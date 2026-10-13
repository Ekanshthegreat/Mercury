#include "test_harness.hpp"
#include "engine/execution/execution.hpp"
#include <cmath>
#include <limits>
using namespace mercury;
namespace {
constexpr Timestamp second = 1'000'000'000;
ExecutionConfig config() { ExecutionConfig c; c.quantity = 600; c.horizon = 4 * second; return c; }
MarketTape empty_tape() { auto p = reference_market(); p.initial_depth_levels = 2; return {p, 4*second, 1, {}}; }
}
MERCURY_TEST(exact_multilevel_fill_cost) {
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, config());
    MERCURY_CHECK_EQ(r.filled, 600);
    MERCURY_CHECK_EQ(r.fill_notional, 700.0); // 500 at ask 1, 100 at ask 2
    MERCURY_CHECK(std::abs(r.cost_ticks - 700.0/600.0) < 1e-12);
}
MERCURY_TEST(sell_cost_has_correct_direction) {
    auto c = config(); c.side = Side::Sell;
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(r.fill_notional, -700.0);
    MERCURY_CHECK(r.cost_ticks > 1.0);
}
MERCURY_TEST(unfilled_quantity_is_not_a_trade) {
    auto c = config(); c.quantity = 2000;
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(r.filled, 1000);
    MERCURY_CHECK_EQ(r.residual, 1000);
    MERCURY_CHECK_EQ(r.fill_rate, 0.5);
    MERCURY_CHECK(r.cost_ticks >= 5.0);
}
MERCURY_TEST(passive_fill_respects_queue_ahead) {
    auto tape = empty_tape();
    tape.events.push_back({second/2, EventType::MarketSell, 100, kNoPrice, 550, 550, 0, 0});
    const auto r = replay_execution(tape, Policy::Passive, config());
    MERCURY_CHECK_EQ(r.actions[1].filled_before, 50);
    MERCURY_CHECK_EQ(r.filled, 600); // passive 50 + deadline closeout 550
    MERCURY_CHECK_EQ(r.fill_notional, 550.0); // -50 + 500*1 + 50*2
}
MERCURY_TEST(twap_budget_and_deadline) {
    const auto r = replay_execution(empty_tape(), Policy::Twap, config());
    MERCURY_CHECK_EQ(r.actions.front().decision.quantity, 150);
    MERCURY_CHECK_EQ(r.actions[1].filled_before, 150);
    MERCURY_CHECK_EQ(r.actions.back().time, 4*second);
    MERCURY_CHECK_EQ(r.filled, 600);
}
MERCURY_TEST(pov_uses_only_observed_market_volume) {
    auto c = config(); c.participation = 0.1;
    auto tape = empty_tape();
    tape.events.push_back({second/2, EventType::MarketSell, 100, kNoPrice, 100, 100, 0, 0});
    const auto r = replay_execution(tape, Policy::Pov, c);
    MERCURY_CHECK_EQ(r.actions[0].decision.quantity, 0);
    MERCURY_CHECK_EQ(r.actions[1].decision.quantity, 10);
}
MERCURY_TEST(all_policies_have_identical_decisions_for_identical_prefixes) {
    auto a = empty_tape(), b = a;
    b.events.push_back({3*second, EventType::MarketBuy, 100, kNoPrice, 800, 800, 0, 0});
    for (const auto p : {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
        const auto x = replay_execution(a, p, config()), y = replay_execution(b, p, config());
        for (std::size_t i = 0; i < 3; ++i) {
            MERCURY_CHECK_EQ(x.actions[i].filled_before, y.actions[i].filled_before);
            MERCURY_CHECK_EQ(x.actions[i].decision.quantity, y.actions[i].decision.quantity);
            MERCURY_CHECK_EQ(x.actions[i].decision.market, y.actions[i].decision.market);
            MERCURY_CHECK_EQ(x.actions[i].decision.price, y.actions[i].decision.price);
        }
    }
}
MERCURY_TEST(reproducible_execution_for_all_policies) {
    const auto tape = generate_tape(reference_market(), 8, 4*second);
    for (const auto p : {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
        const auto a = replay_execution(tape, p, config()), b = replay_execution(tape, p, config());
        MERCURY_CHECK_EQ(a.cost_ticks, b.cost_ticks);
        MERCURY_CHECK_EQ(a.filled + a.residual, a.requested);
        MERCURY_CHECK(a.filled >= 0 && a.filled <= a.requested);
    }
}
MERCURY_TEST(invalid_config_and_tape_are_rejected) {
    auto c = config(); c.quantity = -1;
    MERCURY_CHECK_THROWS(replay_execution(empty_tape(), Policy::Twap, c));
    c = config(); c.participation = std::numeric_limits<double>::quiet_NaN();
    MERCURY_CHECK_THROWS(replay_execution(empty_tape(), Policy::Pov, c));
    auto t = empty_tape(); t.events.push_back({-1, EventType::LimitBid, 99, -1, 50, 0, 0, 0});
    MERCURY_CHECK_THROWS(replay_execution(t, Policy::Twap, config()));
}
MERCURY_MAIN()
