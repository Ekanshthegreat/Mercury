// Mercury — market simulator implementation.
//
// See market_simulator.hpp for the model and the determinism contract.

#include "engine/simulation/market_simulator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mercury {
namespace {

/// Clamp a value into [lo, hi].
[[nodiscard]] double clamp(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/// Keep a multiplier strictly positive so intensities never vanish entirely
/// (a market with exactly zero of some event type would freeze).
[[nodiscard]] double positive(double x) { return x > 1e-9 ? x : 1e-9; }

/// The four-way split of `total` shares across event families, in the enum
/// order the simulator samples. Kept as free constants so the sampling and the
/// factor computation cannot drift apart.
constexpr int kNumEventTypes = 6;

}  // namespace

// ---------------------------------------------------------------------------
// Construction and seeding
// ---------------------------------------------------------------------------

MarketSimulator::MarketSimulator(const SimConfig& config)
    : config_(config), rng_(config.seed) {
    if (!config_.params.is_valid()) {
        throw std::invalid_argument(
            "MarketSimulator: ModelParams failed is_valid()");
    }
    if (config_.horizon < 0) {
        throw std::invalid_argument("MarketSimulator: horizon must be >= 0");
    }
    seed_book();
    hawkes_last_update_ = now_;
}

void MarketSimulator::seed_book() {
    const ModelParams& p = config_.params;
    const Price half = p.initial_half_spread;
    const Quantity size = p.initial_level_size;

    // Mid is at 0 by construction: bid levels run down from -half, ask levels
    // up from +half. Seeding deepest-first means the touch is inserted last,
    // which gives the touch orders the *earliest* priorities — a deliberate,
    // documented choice: it avoids an artefact where the deepest level would
    // otherwise be first in time.
    for (int level = p.initial_depth_levels - 1; level >= 0; --level) {
        const Price bid_price = -half - static_cast<Price>(level);
        const Price ask_price = half + static_cast<Price>(level);
        const OrderId bid_id = next_order_id_++;
        const OrderId ask_id = next_order_id_++;
        book_.submit_limit(Side::Buy, bid_price, size, config_.participant, bid_id);
        book_.submit_limit(Side::Sell, ask_price, size, config_.participant, ask_id);
        live_bids_.push_back(bid_id);
        live_asks_.push_back(ask_id);
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

double MarketSimulator::imbalance() const {
    const Quantity bid_depth = book_.total_quantity(Side::Buy);
    const Quantity ask_depth = book_.total_quantity(Side::Sell);
    const Quantity total = bid_depth + ask_depth;
    if (total <= 0) return 0.0;
    return static_cast<double>(bid_depth - ask_depth) /
           static_cast<double>(total);
}

Price MarketSimulator::current_spread() const {
    return book_.spread().value_or(1);
}

MarketSimulator::IntensityFactors MarketSimulator::current_factors() const {
    return factors();
}

double MarketSimulator::hawkes_boost() const {
    const ModelParams& p = config_.params;
    if (p.hawkes_alpha <= 0.0) return 0.0;
    // Excitation decays exponentially with elapsed virtual time. At exactly
    // alpha == 0 we return 0, so disabling Hawkes is exact, not approximate.
    const double dt_seconds =
        static_cast<double>(now_ - hawkes_last_update_) * 1e-9;
    const double decayed = hawkes_level_ * std::exp(-p.hawkes_beta * dt_seconds);
    // Scale the dimensionless excitation level into an additive intensity. We
    // keep it bounded so total intensity cannot blow up mid-step.
    return clamp(decayed * p.hawkes_alpha, 0.0, 1e6);
}

MarketSimulator::IntensityFactors MarketSimulator::factors() const {
    const ModelParams& p = config_.params;
    const double imb = imbalance();
    const Price spr = current_spread();

    IntensityFactors f;

    // LIMIT: wider spread attracts more passive liquidity.
    f.limit = positive(1.0 + p.limit_spread_sensitivity *
                                 static_cast<double>(spr - 1));

    // CANCEL: the signed state dependence is applied per side at sampling time
    // in step() (cancel_bid_side / cancel_ask_side). This reported factor is
    // only the *magnitude* of that tilt, used by tests and by the Hawkes
    // feedback to gauge how state-dependent cancels currently are; it must not
    // be multiplied back in when sampling or the imbalance would be counted
    // twice.
    f.cancel = positive(1.0 + p.cancel_imbalance_sensitivity * std::fabs(imb));

    // MARKET: as above, the signed split lives in step(). The reported factor is
    // the tilt *magnitude* only. The signed response itself is an antisymmetric
    // log-odds shift added in step(), so the total market intensity is invariant
    // in `market_imbalance_sensitivity` for *any* `flow_imbalance`, not just 0.
    f.market = positive(1.0 + std::fabs(p.market_imbalance_sensitivity) * std::fabs(imb));

    return f;
}

// ---------------------------------------------------------------------------
// Event generation helpers
// ---------------------------------------------------------------------------

Quantity MarketSimulator::draw_limit_size() {
    // Log-normal-ish via exponential mixing would be nicer, but we keep an
    // exponential size (memoryless, always positive) with the configured mean.
    const double mean = config_.params.mean_limit_size;
    const double draw = rng_.exponential(1.0 / mean);
    const Quantity q = static_cast<Quantity>(draw) + 1;
    return q > 0 ? q : 1;
}

Quantity MarketSimulator::draw_market_size() {
    const double mean = config_.params.mean_market_size;
    const double draw = rng_.exponential(1.0 / mean);
    const Quantity q = static_cast<Quantity>(draw) + 1;
    return q > 0 ? q : 1;
}

Price MarketSimulator::choose_limit_price(Side side) {
    const ModelParams& p = config_.params;

    // Place relative to the touch. With high probability the order improves or
    // joins the touch; occasionally it is placed a few ticks deeper, which gives
    // the book its depth profile.
    const auto touch = book_.best_price(side);
    if (!touch.has_value()) {
        // Empty side: fall back to a small offset around the opposite touch.
        const Price opp = book_.best_price(opposite(side)).value_or(0);
        return side == Side::Buy ? opp - 1 : opp + 1;
    }

    // Offset in ticks *behind* the touch (0 = join the touch, 1 = one tick
    // worse, ...). Exponential-ish tail keeps most size at the touch.
    const double u = rng_.uniform_double();
    int offset = 0;
    if (u > 0.60) offset = 1;
    if (u > 0.85) offset = 2;
    if (u > 0.95) offset = 3 + static_cast<int>(rng_.uniform_u64(4));

    // A price-improving order (better than the touch) only makes sense if it
    // does not cross; we keep generated orders non-crossing by construction so
    // that "market" events are the only liquidity takers. This makes the
    // attribution of trades to event types clean.
    (void)p;
    return side == Side::Buy ? *touch - static_cast<Price>(offset)
                             : *touch + static_cast<Price>(offset);
}

// ---------------------------------------------------------------------------
// Event application
// ---------------------------------------------------------------------------

Quantity MarketSimulator::do_cancel(Side side, EventType type) {
    std::vector<OrderId>& live = side == Side::Buy ? live_bids_ : live_asks_;
    if (live.empty()) return 0;

    // Pick a uniformly random resting order on this side. Retry a bounded
    // number of times to skip ids that have since been fully filled.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::size_t idx =
            static_cast<std::size_t>(rng_.uniform_u64(live.size()));
        const OrderId id = live[idx];
        const Order* ord = book_.find(id);
        if (ord != nullptr && ord->remaining > 0) {
            // Snapshot before `cancel` invalidates the pointer (the order is
            // erased from the book's storage).
            const Quantity q = ord->remaining;
            const Price px = ord->price;
            if (book_.cancel(id)) {
                live[idx] = live.back();
                live.pop_back();
                stats_.cancel_orders += 1;
                stats_.shares_cancelled += q;
                if (config_.record_events) {
                    events_.push_back(SimEvent{now_, type, id, px, q, 0, 0.0, 0.0});
                }
                return q;
            }
        }
        // Stale id: drop it and try again.
        live[idx] = live.back();
        live.pop_back();
        if (live.empty()) return 0;
    }
    return 0;
}

Quantity MarketSimulator::do_limit(Side side, EventType type) {
    const Price price = choose_limit_price(side);
    const Quantity qty = draw_limit_size();
    const OrderId id = next_order_id_++;

    const SubmitResult res =
        book_.submit_limit(side, price, qty, config_.participant, id, now_);

    stats_.limit_orders += 1;
    if (res.filled > 0) {
        stats_.shares_traded += res.filled;
    }
    if (res.resting > 0) {
        (side == Side::Buy ? live_bids_ : live_asks_).push_back(id);
    }
    if (config_.record_events) {
        events_.push_back(
            SimEvent{now_, type, id, price, qty, res.filled, 0.0, 0.0});
    }
    return res.filled;
}

Quantity MarketSimulator::do_market(Side side, EventType type) {
    const Quantity qty = draw_market_size();
    const OrderId id = next_order_id_++;
    const SubmitResult res = book_.submit_market(side, qty, config_.participant, id, now_);

    stats_.market_orders += 1;
    stats_.shares_traded += res.filled;
    if (config_.record_events) {
        events_.push_back(
            SimEvent{now_, type, id, kNoPrice, qty, res.filled, 0.0, 0.0});
    }
    return res.filled;
}

// ---------------------------------------------------------------------------
// The main step
// ---------------------------------------------------------------------------

bool MarketSimulator::step() {
    if (now_ >= config_.horizon) return false;
    if (stats_.event_count >= 2'000'000) throw std::runtime_error("Simulation event budget exceeded");

    const ModelParams& p = config_.params;

    // --- Intensities at the current (piecewise-constant) state ---------------
    const double imb = imbalance();
    const double boost = hawkes_boost();
    const IntensityFactors f = factors();

    // Per-event-type intensity. The state-dependence is signed here: cancels
    // lean to the thin side, market orders lean with the queue pressure, and
    // the flow-imbalance parameter biases the buy/sell split of *all* flow.
    const double flow_up = 0.5 * (1.0 + p.flow_imbalance);
    const double flow_dn = 1.0 - flow_up;

    // Cancel leans to the thin side: thin bid (imb < 0) -> more bid cancels.
    // This channel is governed *solely* by `cancel_imbalance_sensitivity`;
    // `market_imbalance_sensitivity` has its own, separate effect below.
    const double cancel_k = p.cancel_imbalance_sensitivity;
    const double cancel_bid_side = 1.0 + cancel_k * clamp(-imb, -1.0, 1.0);
    const double cancel_ask_side = 1.0 + cancel_k * clamp(imb, -1.0, 1.0);

    // Market flow responds to the *same* imbalance. `market_imbalance_sensitivity`
    // is signed: negative = contrarian, positive = momentum. `imb > 0` means a
    // thick *bid*, so contrarian means "sell into the thick bid": when imb > 0
    // the sell side must rise, i.e. buy ~ 1 - s*imb, sell ~ 1 + s*imb with s < 0.
    // The buy-side pull is therefore the *negative* of the sensitivity.
    //
    // The imbalance response is a *log-odds shift*, added to the directional
    // bias in the same additive space rather than multiplied into it. A
    // multiplicative split cannot coexist with `flow_imbalance`: when the two
    // channels oppose (the usual case, since `mis` reads the book's imbalance
    // while `flow_imbalance` is an independent exogenous dial) the products
    // fight and the coupling inverts the requested direction. Adding in
    // log-odds keeps the two effects independent and monotone in every cell.
    //
    // `mis` still only *reallocates* volume between the sides. The shift is
    // applied in log-odds space and then *re-centred* so that the arithmetic
    // mean of the two log-intensities is unchanged. Re-centring (rather than a
    // multiplicative split) is what makes the total market intensity exactly
    // invariant in `mis` for any `flow_imbalance`: a multiplicative split
    // cannot do this, because the two channels then compound.
    //
    // The Hawkes boost is treated as part of the same directional bias so that
    // the baseline stays balanced: with `flow_imbalance = 0` and `mis = 0` the
    // market legs are equal, and turning the coupling on leaves their *sum*
    // fixed while moving their difference. Any Phase 7 assertion of neutrality
    // at `mis = 0` therefore holds exactly, not just up to the Hawkes term.
    const double mis = clamp(p.market_imbalance_sensitivity, -1.0, 1.0);
    const double mkt_bid_pull = mis * clamp(imb, -1.0, 1.0);

    // Directional bias in log-odds, including the Hawkes excitation on both
    // legs (the boost is split across the two market sides by `flow_up/dn`).
    const double log_up = std::log(flow_up + 1e-12) - mkt_bid_pull * 0.5;
    const double log_dn = std::log(flow_dn + 1e-12) + mkt_bid_pull * 0.5;

    // Re-centre so the mean log-intensity is preserved exactly.
    const double log_mid = 0.5 * (log_up + log_dn);
    const double mkt_up = std::exp(log_up - log_mid);
    const double mkt_dn = std::exp(log_dn - log_mid);

    double intens[kNumEventTypes];
    intens[static_cast<int>(EventType::LimitBid)] =
        positive(p.limit_rate * f.limit * flow_up) + boost * flow_up;
    intens[static_cast<int>(EventType::LimitAsk)] =
        positive(p.limit_rate * f.limit * flow_dn) + boost * flow_dn;
    intens[static_cast<int>(EventType::CancelBid)] =
        positive(p.cancel_rate * cancel_bid_side) + boost * 0.5;
    intens[static_cast<int>(EventType::CancelAsk)] =
        positive(p.cancel_rate * cancel_ask_side) + boost * 0.5;
    // Market legs: `market_rate` times the re-centred log-odds split, with the
    // Hawkes excitation added on top of the same legs.
    intens[static_cast<int>(EventType::MarketBuy)] =
        positive(p.market_rate * mkt_up) + boost * mkt_up;
    intens[static_cast<int>(EventType::MarketSell)] =
        positive(p.market_rate * mkt_dn) + boost * mkt_dn;

    double total = 0.0;
    for (double x : intens) total += x;
    if (!(total > 0.0)) {
        // Nothing can happen; jump to the horizon to avoid a busy loop.
        now_ = config_.horizon;
        return false;
    }

    // --- Draw the waiting time (exact for piecewise-constant intensity) ------
    const double dt_seconds = rng_.exponential(total);
    const Timestamp dt_ns = static_cast<Timestamp>(dt_seconds * 1e9) + 1;
    if (dt_ns > config_.horizon - now_) {
        now_ = config_.horizon;
        return false;
    }
    now_ += dt_ns;

    // --- Choose the event type by inverse-CDF sampling ----------------------
    const double target = rng_.uniform_double() * total;
    double accum = 0.0;
    int chosen = kNumEventTypes - 1;
    for (int i = 0; i < kNumEventTypes; ++i) {
        accum += intens[i];
        if (target < accum) {
            chosen = i;
            break;
        }
    }
    const EventType type = static_cast<EventType>(chosen);

    // --- Apply it -----------------------------------------------------------
    const double mid_before = book_.mid_price().value_or(0.0);

    // The fixed quantities returned by the helpers are already folded into
    // `stats_` inside them; here we only need the side effect.
    const std::size_t log_size_before = events_.size();
    switch (type) {
        case EventType::LimitBid:
            (void)do_limit(Side::Buy, type);
            break;
        case EventType::LimitAsk:
            (void)do_limit(Side::Sell, type);
            break;
        case EventType::CancelBid:
            (void)do_cancel(Side::Buy, type);
            break;
        case EventType::CancelAsk:
            (void)do_cancel(Side::Sell, type);
            break;
        case EventType::MarketBuy:
            (void)do_market(Side::Buy, type);
            break;
        case EventType::MarketSell:
            do_market(Side::Sell, type);
            break;
    }

    // --- Hawkes bookkeeping and mid-change statistics -----------------------
    hawkes_level_ = hawkes_level_ * std::exp(-p.hawkes_beta *
                                             (dt_seconds)) +
                    1.0;
    hawkes_last_update_ = now_;

    const double mid_after = book_.mid_price().value_or(0.0);
    const double dmid = mid_after - mid_before;
    if (dmid != 0.0) {
        stats_.sum_squared_mid_change += dmid * dmid;
        stats_.mid_change_count += 1;
    }
    if (config_.record_events && events_.size() > log_size_before) {
        events_.back().mid_before = mid_before;
        events_.back().mid_after = mid_after;
    }

    stats_.event_count += 1;
    return true;
}

std::uint64_t MarketSimulator::run() {
    std::uint64_t applied = 0;
    while (step()) {
        ++applied;
    }
    return applied;
}

}  // namespace mercury
