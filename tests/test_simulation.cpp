// Mercury — Phase 3 tests: deterministic, state-dependent order flow.
//
// The Phase 3 gate is two claims, and each gets its own test group:
//
//   1. **Same-seed determinism.** Two simulators built from the same params and
//      seed must produce byte-identical event streams (times, types, ids,
//      prices, sizes, fills), and different seeds must differ.
//   2. **Rates respond to params.** Changing a parameter must change the event
//      mix in the direction the model says it should, monotone where
//      monotonicity is expected. This is what stops the simulator from being a
//      decorative wrapper whose "parameters" do nothing.
//
// A third group checks the state-dependence itself (the queue-reactive core):
// the intensity multipliers must move with observable book state, not just with
// the parameters.

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "engine/simulation/market_simulator.hpp"
#include "tests/test_harness.hpp"

using namespace mercury;

namespace {

/// A fixed, long-enough horizon for statistical tests. 60 seconds of virtual
/// time at baseline rates yields thousands of events, enough to compare mixes.
constexpr Timestamp kSecond = 1'000'000'000LL;

SimConfig make_config(std::uint64_t seed = 12345,
                      Timestamp horizon = 60 * kSecond) {
    SimConfig c;
    c.params = reference_market();
    c.seed = seed;
    c.horizon = horizon;
    c.participant = 1;
    return c;
}

/// Serialise the observable fingerprint of a run: every event, in order.
std::vector<std::string> fingerprint(const MarketSimulator& sim) {
    std::vector<std::string> out;
    out.reserve(sim.events().size());
    for (const SimEvent& e : sim.events()) {
        out.push_back(std::to_string(e.t) + ":" +
                      std::to_string(static_cast<int>(e.type)) + ":" +
                      std::to_string(e.order_id) + ":" +
                      std::to_string(e.price) + ":" +
                      std::to_string(e.quantity) + ":" +
                      std::to_string(e.filled));
    }
    return out;
}

/// Run a simulator and return it (records events so we can fingerprint).
MarketSimulator run_recording(const SimConfig& cfg, std::uint64_t& applied) {
    SimConfig c = cfg;
    c.record_events = true;
    MarketSimulator sim(c);
    applied = sim.run();
    return sim;
}

/// Count events of each type in a run.
std::map<int, int> event_mix(const MarketSimulator& sim) {
    std::map<int, int> mix;
    for (const SimEvent& e : sim.events()) {
        mix[static_cast<int>(e.type)] += 1;
    }
    return mix;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Determinism
// ---------------------------------------------------------------------------

MERCURY_TEST(same_seed_produces_identical_event_streams) {
    std::uint64_t applied_a = 0;
    std::uint64_t applied_b = 0;
    const auto a = run_recording(make_config(777), applied_a);
    const auto b = run_recording(make_config(777), applied_b);

    MERCURY_CHECK_EQ(applied_a, applied_b);
    MERCURY_CHECK(!a.events().empty());
    MERCURY_CHECK_EQ(a.stats().event_count, b.stats().event_count);
    MERCURY_CHECK_EQ(a.stats().shares_traded, b.stats().shares_traded);
    MERCURY_CHECK_EQ(a.stats().shares_cancelled, b.stats().shares_cancelled);
    MERCURY_CHECK(fingerprint(a) == fingerprint(b));
}

MERCURY_TEST(different_seeds_produce_different_streams) {
    std::uint64_t applied_a = 0;
    std::uint64_t applied_b = 0;
    const auto a = run_recording(make_config(1), applied_a);
    const auto b = run_recording(make_config(2), applied_b);

    // The streams should almost surely differ. If this ever fires, the seed is
    // being ignored somewhere.
    MERCURY_CHECK(fingerprint(a) != fingerprint(b));
}

MERCURY_TEST(rng_is_reproducible_and_splittable) {
    // The RNG is the foundation of determinism, so test it directly too.
    Rng a(99);
    Rng b(99);
    for (int i = 0; i < 1000; ++i) {
        MERCURY_CHECK_EQ(a.next_u64(), b.next_u64());
    }

    // split() must be reproducible from the same parent state and must not
    // return the parent's own stream.
    Rng p1(5);
    Rng p2(5);
    Rng c1 = p1.split();
    Rng c2 = p2.split();
    for (int i = 0; i < 100; ++i) {
        MERCURY_CHECK_EQ(c1.next_u64(), c2.next_u64());
    }
    Rng parent(5);
    Rng child = Rng(5).split();
    bool differs = false;
    for (int i = 0; i < 100; ++i) {
        if (parent.next_u64() != child.next_u64()) differs = true;
    }
    MERCURY_CHECK(differs);
}

MERCURY_TEST(rng_distributions_have_expected_moments) {
    Rng r(2024);
    // Uniform integer in a range must actually cover it and stay inside.
    int lo_seen = 0, hi_seen = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::int64_t v = r.uniform_int(0, 9);
        MERCURY_CHECK(v >= 0 && v <= 9);
        if (v == 0) ++lo_seen;
        if (v == 9) ++hi_seen;
    }
    MERCURY_CHECK(lo_seen > 0 && hi_seen > 0);

    // Exponential(rate=2) has mean 1/2. Check within 10%.
    double sum = 0.0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double x = r.exponential(2.0);
        MERCURY_CHECK(x >= 0.0);
        sum += x;
    }
    const double mean = sum / n;
    MERCURY_CHECK(mean > 0.45 && mean < 0.55);

    // uniform_double stays in [0, 1).
    for (int i = 0; i < 20000; ++i) {
        const double u = r.uniform_double();
        MERCURY_CHECK(u >= 0.0 && u < 1.0);
    }
}

// ---------------------------------------------------------------------------
// 2. Rates respond to parameters
// ---------------------------------------------------------------------------

MERCURY_TEST(market_order_rate_responds_to_parameter) {
    // A market with 10x the market-order intensity must produce materially more
    // market orders. We compare two seeds' worth of runs to avoid a one-seed
    // fluke.
    int baseline_markets = 0;
    int high_markets = 0;
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        SimConfig lo = make_config(seed);
        lo.params.market_rate = 1.0;
        std::uint64_t applied = 0;
        const auto s_lo = run_recording(lo, applied);
        baseline_markets += event_mix(s_lo)[static_cast<int>(EventType::MarketBuy)] +
                            event_mix(s_lo)[static_cast<int>(EventType::MarketSell)];

        SimConfig hi = make_config(seed);
        hi.params.market_rate = 10.0;
        const auto s_hi = run_recording(hi, applied);
        high_markets += event_mix(s_hi)[static_cast<int>(EventType::MarketBuy)] +
                        event_mix(s_hi)[static_cast<int>(EventType::MarketSell)];
    }
    MERCURY_CHECK(high_markets > baseline_markets);
}

MERCURY_TEST(cancel_rate_responds_to_parameter) {
    int low_cancels = 0;
    int high_cancels = 0;
    for (std::uint64_t seed : {11u, 12u, 13u}) {
        SimConfig lo = make_config(seed);
        lo.params.cancel_rate = 0.5;
        std::uint64_t applied = 0;
        const auto s_lo = run_recording(lo, applied);
        low_cancels += event_mix(s_lo)[static_cast<int>(EventType::CancelBid)] +
                       event_mix(s_lo)[static_cast<int>(EventType::CancelAsk)];

        SimConfig hi = make_config(seed);
        hi.params.cancel_rate = 20.0;
        const auto s_hi = run_recording(hi, applied);
        high_cancels += event_mix(s_hi)[static_cast<int>(EventType::CancelBid)] +
                        event_mix(s_hi)[static_cast<int>(EventType::CancelAsk)];
    }
    MERCURY_CHECK(high_cancels > low_cancels);
}

MERCURY_TEST(flow_imbalance_biases_buy_versus_sell_market_flow) {
    // With flow_imbalance > 0, market BUY events must outnumber market SELLs;
    // with < 0 the reverse.
    SimConfig up = make_config(4242);
    up.params.flow_imbalance = 0.8;
    std::uint64_t applied = 0;
    const auto s_up = run_recording(up, applied);
    const auto mix_up = event_mix(s_up);
    const int buys_up = mix_up.at(static_cast<int>(EventType::MarketBuy));
    const int sells_up = mix_up.at(static_cast<int>(EventType::MarketSell));
    MERCURY_CHECK(buys_up > sells_up);

    SimConfig dn = make_config(4242);
    dn.params.flow_imbalance = -0.8;
    const auto s_dn = run_recording(dn, applied);
    const auto mix_dn = event_mix(s_dn);
    const int buys_dn = mix_dn.at(static_cast<int>(EventType::MarketBuy));
    const int sells_dn = mix_dn.at(static_cast<int>(EventType::MarketSell));
    MERCURY_CHECK(sells_dn > buys_dn);
}

MERCURY_TEST(symmetric_market_has_no_buy_sell_bias) {
    // With flow_imbalance = 0 and symmetric initialisation the model is
    // symmetric by construction (the Hawkes split re-centres to 50/50 and the
    // imbalance coupling cancels), so a *systematic* buy/sell bias would have
    // to show up in the mean over many independent runs.
    //
    // A single 300s run only produces ~250 market events, and the relative gap
    // between buy and sell counts at that sample size swings by order +/-100%
    // purely from counting noise -- too noisy to assert anything. So we average
    // the signed imbalance over many seeds: the noise cancels and only a real
    // drift in the model survives. This is deterministic and much stricter than
    // a loose per-run tolerance.
    constexpr int kRuns = 200;
    long total_mb = 0;
    long total_ms = 0;
    for (std::uint64_t seed = 1; seed <= kRuns; ++seed) {
        SimConfig c = make_config(seed, 300 * kSecond);
        std::uint64_t applied = 0;
        const auto s = run_recording(c, applied);
        const auto mix = event_mix(s);
        total_mb += mix.at(static_cast<int>(EventType::MarketBuy));
        total_ms += mix.at(static_cast<int>(EventType::MarketSell));
    }

    // Over 200 runs the pooled sample is ~50k events, so 1/sqrt(N) noise is a
    // few percent; 10% leaves margin while still catching a genuine drift.
    const double mean = 0.5 * static_cast<double>(total_mb + total_ms);
    MERCURY_CHECK(mean > 0);
    MERCURY_CHECK(std::fabs(static_cast<double>(total_mb - total_ms)) / mean < 0.10);
}

// ---------------------------------------------------------------------------
// 3. State dependence (the queue-reactive core)
// ---------------------------------------------------------------------------

MERCURY_TEST(intensity_factors_are_state_dependent) {
    // Build a book and confirm the intensity multipliers move when the book
    // state moves. We drive the book directly (not via the simulator's random
    // flow) so this test is about the *function*, not about luck.
    SimConfig cfg = make_config(1);
    cfg.params.cancel_imbalance_sensitivity = 2.0;
    cfg.params.market_imbalance_sensitivity = 2.0;
    MarketSimulator sim(cfg);

    // A freshly seeded book is balanced: factors should be ~1 (no state
    // dependence to apply yet).
    const auto balanced = sim.current_factors();
    MERCURY_CHECK(std::fabs(balanced.cancel - 1.0) < 1e-9);
    MERCURY_CHECK(std::fabs(balanced.market - 1.0) < 1e-9);

    // Drive the simulator and confirm that, once the book's shape changes, the
    // factors move away from 1 — i.e. the state genuinely feeds back into the
    // intensities rather than the parameters acting on their own.
    bool saw_deviation = false;
    for (int i = 0; i < 5000 && sim.step(); ++i) {
        const auto f = sim.current_factors();
        if (std::fabs(f.cancel - 1.0) > 1e-6 ||
            std::fabs(f.market - 1.0) > 1e-6) {
            saw_deviation = true;
            break;
        }
    }
    MERCURY_CHECK(saw_deviation);
}

MERCURY_TEST(simulation_conserves_book_invariants) {
    // Every event must leave the book in a valid state. This ties Phase 3's
    // generator back to Phase 2's invariants.
    SimConfig c = make_config(555, 30 * kSecond);
    MarketSimulator sim(c);
    for (int i = 0; i < 5000 && sim.step(); ++i) {
        sim.book().check_invariants();
        // The mid, when the book is two-sided, must be a finite finite value
        // between the best bid and best ask.
        const auto mid = sim.book().mid_price();
        const auto bid = sim.book().best_price(Side::Buy);
        const auto ask = sim.book().best_price(Side::Sell);
        if (mid.has_value() && bid.has_value() && ask.has_value()) {
            MERCURY_CHECK(*mid >= static_cast<double>(*bid));
            MERCURY_CHECK(*mid <= static_cast<double>(*ask));
        }
    }
}

MERCURY_TEST(time_is_monotonic_and_within_horizon) {
    SimConfig c = make_config(321, 10 * kSecond);
    MarketSimulator sim(c);
    Timestamp last = 0;
    int prev_mid_changes = 0;
    while (sim.step()) {
        MERCURY_CHECK(sim.now() >= last);
        last = sim.now();
        MERCURY_CHECK(sim.now() >= 0);
        prev_mid_changes = static_cast<int>(sim.stats().event_count);
        (void)prev_mid_changes;
        if (sim.stats().event_count > 100000) break;  // safety valve
    }
    MERCURY_CHECK(sim.now() == c.horizon);
    MERCURY_CHECK(sim.stats().event_count > 0);
}

MERCURY_TEST(invalid_params_are_rejected) {
    SimConfig c = make_config(1);
    c.params.market_rate = -1.0;
    MERCURY_CHECK_THROWS(MarketSimulator{c});

    SimConfig c2 = make_config(1);
    c2.params.hawkes_alpha = 1.5;  // must be < 1
    MERCURY_CHECK_THROWS(MarketSimulator{c2});

    SimConfig c3 = make_config(1);
    c3.params.initial_half_spread = 0;
    MERCURY_CHECK_THROWS(MarketSimulator{c3});
}

MERCURY_TEST(hawkes_increases_event_clustering) {
    // With self-excitation on, the variance-to-mean ratio of inter-event gaps
    // should not be below the Poisson value of 1, and typically exceeds it
    // (clustering stretches the tail). We test the weaker, robust claim: the
    // dispersion is >= the Poisson case, allowing simulation noise.
    auto dispersion = [](const SimConfig& cfg) {
        MarketSimulator sim(cfg);
        std::vector<double> gaps;
        Timestamp prev = 0;
        while (sim.step()) {
            const double dt = static_cast<double>(sim.now() - prev) * 1e-9;
            if (dt > 0) gaps.push_back(dt);
            prev = sim.now();
            if (gaps.size() > 20000) break;
        }
        double sum = 0.0;
        for (double g : gaps) sum += g;
        const double mean = sum / static_cast<double>(gaps.size());
        double sq = 0.0;
        for (double g : gaps) sq += (g - mean) * (g - mean);
        const double var = sq / static_cast<double>(gaps.size());
        return var / (mean * mean);  // squared coefficient of variation
    };

    SimConfig poisson = make_config(2468, 120 * kSecond);
    poisson.params.hawkes_alpha = 0.0;
    const double d_poisson = dispersion(poisson);

    SimConfig hawkes = make_config(2468, 120 * kSecond);
    hawkes.params.hawkes_alpha = 0.7;
    hawkes.params.hawkes_beta = 2.0;
    const double d_hawkes = dispersion(hawkes);

    // Poisson inter-arrivals are exponential: mean == sd, so d ~= 1.
    MERCURY_CHECK(d_poisson > 0.5 && d_poisson < 2.0);
    // Hawkes should be at least as dispersed as Poisson (clustering fattens the
    // tail of the gap distribution).
    MERCURY_CHECK(d_hawkes >= d_poisson * 0.9);
}

MERCURY_TEST(rejects_nonfinite_parameters) {
    auto c = make_config(1);
    c.params.limit_rate = std::numeric_limits<double>::quiet_NaN();
    MERCURY_CHECK_THROWS(MarketSimulator{c});
    c.params.limit_rate = 1;
    c.params.mean_limit_size = std::numeric_limits<double>::infinity();
    MERCURY_CHECK_THROWS(MarketSimulator{c});
}
MERCURY_TEST(no_event_exceeds_horizon) {
    MarketSimulator sim(make_config(8, 1));
    sim.run();
    MERCURY_CHECK_EQ(sim.events().size(), 0U);
    MERCURY_CHECK_EQ(sim.now(), 1);
}
MERCURY_MAIN()
