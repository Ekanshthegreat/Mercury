#include <cmath>
#include <limits>

#include "engine/execution/execution.hpp"
#include "test_harness.hpp"
using namespace mercury;
namespace {
constexpr Timestamp second = 1'000'000'000;
ExecutionConfig config() {
    ExecutionConfig c;
    c.quantity = 600;
    c.horizon = 4 * second;
    return c;
}
MarketTape empty_tape() {
    auto p = reference_market();
    p.initial_depth_levels = 2;
    return {p, 4 * second, 1, {}};
}
}  // namespace
MERCURY_TEST(exact_multilevel_fill_cost) {
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, config());
    MERCURY_CHECK_EQ(r.filled, 600);
    MERCURY_CHECK_EQ(r.fill_notional, 700.0);  // 500 at ask 1, 100 at ask 2
    MERCURY_CHECK(std::abs(r.cost_ticks - 700.0 / 600.0) < 1e-12);
}
MERCURY_TEST(sell_cost_has_correct_direction) {
    auto c = config();
    c.side = Side::Sell;
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(r.fill_notional, -700.0);
    MERCURY_CHECK(r.cost_ticks > 1.0);
}
MERCURY_TEST(unfilled_quantity_is_not_a_trade) {
    auto c = config();
    c.quantity = 2000;
    const auto r = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(r.filled, 1000);
    MERCURY_CHECK_EQ(r.residual, 1000);
    MERCURY_CHECK_EQ(r.fill_rate, 0.5);
    MERCURY_CHECK(r.cost_ticks >= 5.0);
}
MERCURY_TEST(passive_fill_respects_queue_ahead) {
    auto tape = empty_tape();
    tape.events.push_back({second / 2, EventType::MarketSell, 100, kNoPrice, 550, 550, 0, 0});
    const auto r = replay_execution(tape, Policy::Passive, config());
    MERCURY_CHECK_EQ(r.actions[1].filled_before, 50);
    MERCURY_CHECK_EQ(r.filled, 600);           // passive 50 + deadline closeout 550
    MERCURY_CHECK_EQ(r.fill_notional, 550.0);  // -50 + 500*1 + 50*2
}
MERCURY_TEST(twap_budget_and_deadline) {
    const auto r = replay_execution(empty_tape(), Policy::Twap, config());
    MERCURY_CHECK_EQ(r.actions.front().decision.quantity, 150);
    MERCURY_CHECK_EQ(r.actions[1].filled_before, 150);
    MERCURY_CHECK_EQ(r.actions.back().time, 4 * second);
    MERCURY_CHECK_EQ(r.filled, 600);
}
MERCURY_TEST(pov_uses_only_observed_market_volume) {
    auto c = config();
    c.participation = 0.1;
    auto tape = empty_tape();
    tape.events.push_back({second / 2, EventType::MarketSell, 100, kNoPrice, 100, 100, 0, 0});
    const auto r = replay_execution(tape, Policy::Pov, c);
    MERCURY_CHECK_EQ(r.actions[0].decision.quantity, 0);
    MERCURY_CHECK_EQ(r.actions[1].decision.quantity, 10);
}
MERCURY_TEST(all_policies_have_identical_decisions_for_identical_prefixes) {
    auto a = empty_tape(), b = a;
    b.events.push_back({3 * second, EventType::MarketBuy, 100, kNoPrice, 800, 800, 0, 0});
    for (const auto p :
         {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
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
    const auto tape = generate_tape(reference_market(), 8, 4 * second);
    for (const auto p :
         {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
        const auto a = replay_execution(tape, p, config()), b = replay_execution(tape, p, config());
        MERCURY_CHECK_EQ(a.cost_ticks, b.cost_ticks);
        MERCURY_CHECK_EQ(a.filled + a.residual, a.requested);
        MERCURY_CHECK(a.filled >= 0 && a.filled <= a.requested);
    }
}
MERCURY_TEST(invalid_config_and_tape_are_rejected) {
    auto c = config();
    c.quantity = -1;
    MERCURY_CHECK_THROWS(replay_execution(empty_tape(), Policy::Twap, c));
    c = config();
    c.participation = std::numeric_limits<double>::quiet_NaN();
    MERCURY_CHECK_THROWS(replay_execution(empty_tape(), Policy::Pov, c));
    auto t = empty_tape();
    t.events.push_back({-1, EventType::LimitBid, 99, -1, 50, 0, 0, 0});
    MERCURY_CHECK_THROWS(replay_execution(t, Policy::Twap, config()));
}
MERCURY_TEST(fill_trace_reconstructs_quantity_notional_and_liquidity) {
    auto tape = empty_tape();
    tape.events.push_back({second / 2, EventType::MarketSell, 100, kNoPrice, 550, 550, 0, 0});
    const auto r = replay_execution(tape, Policy::Passive, config());
    Quantity quantity = 0, passive = 0;
    double notional = 0;
    Timestamp previous = 0;
    for (const auto& fill : r.fills) {
        quantity += fill.quantity;
        notional += static_cast<double>(fill.quantity) * static_cast<double>(fill.price);
        if (fill.passive) passive += fill.quantity;
        MERCURY_CHECK(fill.time >= previous && fill.time <= config().horizon);
        previous = fill.time;
    }
    MERCURY_CHECK_EQ(quantity, r.filled);
    MERCURY_CHECK_EQ(notional, r.fill_notional);
    MERCURY_CHECK_EQ(passive, 50);
}
MERCURY_TEST(fees_apply_to_executed_shares_and_rebates_have_correct_sign) {
    auto tape = empty_tape();
    tape.events.push_back({second / 2, EventType::MarketSell, 100, kNoPrice, 550, 550, 0, 0});
    auto c = config();
    c.maker_fee = -0.5;
    c.taker_fee = 1;
    const auto base = replay_execution(tape, Policy::Passive, config());
    const auto charged = replay_execution(tape, Policy::Passive, c);
    MERCURY_CHECK_EQ(charged.fee_notional, 525.0);
    MERCURY_CHECK(std::abs(charged.cost_ticks - base.cost_ticks - 525.0 / 600.0) < 1e-12);
    c.quantity = 2000;
    c.taker_fee = 1;
    c.maker_fee = 0;
    const auto partial = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(partial.fee_notional, 1000.0);
    MERCURY_CHECK_EQ(partial.residual, 1000);
}
MERCURY_TEST(submission_latency_changes_arrival_book_and_equal_time_priority) {
    auto tape = empty_tape();
    // External buy consumes ask 1 exactly when the delayed buy arrives.
    tape.events.push_back({second / 2, EventType::MarketBuy, 100, kNoPrice, 500, 500, 0, 0});
    auto c = config(); c.quantity = 100;
    const auto immediate = replay_execution(tape, Policy::Aggressive, c);
    c.submission_latency = second / 2;
    const auto delayed = replay_execution(tape, Policy::Aggressive, c);
    MERCURY_CHECK_EQ(immediate.fill_notional, 100.0);
    MERCURY_CHECK_EQ(delayed.fill_notional, 200.0);
    MERCURY_CHECK_EQ(delayed.fills.front().time, second / 2);
    MERCURY_CHECK_EQ(delayed.actions.front().time, 0);
    MERCURY_CHECK_EQ(delayed.actions.front().arrival_time, second / 2);
}
MERCURY_TEST(long_latency_keeps_one_submission_in_flight_and_expires_at_deadline) {
    auto c = config(); c.submission_latency = 2 * second;
    const auto r = replay_execution(empty_tape(), Policy::Twap, c);
    MERCURY_CHECK_EQ(r.actions.size(), 3u);  // t=0, 2, and mandatory t=4 closeout
    MERCURY_CHECK_EQ(r.actions[0].arrival_time, 2 * second);
    MERCURY_CHECK_EQ(r.actions[1].arrival_time, -1);  // t=4 arrival is preempted
    MERCURY_CHECK_EQ(r.actions[1].arrived_quantity, 0);
    MERCURY_CHECK_EQ(r.actions.back().arrival_time, c.horizon);
    MERCURY_CHECK_EQ(r.filled, 600);
    c.submission_latency = c.horizon + second;
    const auto expired = replay_execution(empty_tape(), Policy::Aggressive, c);
    MERCURY_CHECK_EQ(expired.actions.front().arrival_time, -1);
    MERCURY_CHECK_EQ(expired.fills.front().time, c.horizon);
}
MERCURY_TEST(delayed_passive_order_misses_flow_before_arrival) {
    auto tape = empty_tape();
    tape.events.push_back({second / 2, EventType::MarketSell, 100, kNoPrice, 550, 550, 0, 0});
    auto c = config(); c.submission_latency = 3 * second / 4;
    const auto r = replay_execution(tape, Policy::Passive, c);
    Quantity maker = 0;
    for (const auto& fill : r.fills) if (fill.passive) maker += fill.quantity;
    MERCURY_CHECK_EQ(maker, 0);
    MERCURY_CHECK_EQ(r.filled + r.residual, c.quantity);
}
MERCURY_TEST(latency_does_not_leak_future_events_into_decisions) {
    auto a = empty_tape(), b = a;
    b.events.push_back({second / 2, EventType::MarketBuy, 100, kNoPrice, 500, 500, 0, 0});
    auto c = config(); c.submission_latency = second;
    for (auto policy : {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive, Policy::Adaptive}) {
        const auto x = replay_execution(a, policy, c), y = replay_execution(b, policy, c);
        MERCURY_CHECK_EQ(x.actions[0].decision.quantity, y.actions[0].decision.quantity);
        MERCURY_CHECK_EQ(x.actions[0].decision.price, y.actions[0].decision.price);
        MERCURY_CHECK_EQ(x.actions[0].decision.market, y.actions[0].decision.market);
        MERCURY_CHECK_EQ(y.filled + y.residual, c.quantity);
    }
    c.submission_latency = -1;
    MERCURY_CHECK_THROWS(replay_execution(a, Policy::Twap, c));
    c.submission_latency = 3600'000'000'001LL;
    MERCURY_CHECK_THROWS(replay_execution(a, Policy::Twap, c));
}
MERCURY_MAIN()
