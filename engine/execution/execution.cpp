#include "engine/execution/execution.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
namespace mercury {
namespace {
constexpr OrderId algo_base = OrderId{1} << 63;
void validate(const ExecutionConfig& c) {
    if (c.quantity <= 0 || c.quantity > 1'000'000'000 || c.horizon <= 0 ||
        c.horizon > 3600'000'000'000LL || c.interval <= 0 || c.interval > c.horizon ||
        c.submission_latency < 0 || c.submission_latency > 3600'000'000'000LL ||
        c.horizon / c.interval > 100000 || !std::isfinite(c.participation) ||
        c.participation <= 0 || c.participation > 1 || !std::isfinite(c.maker_fee) ||
        std::abs(c.maker_fee) > 1000 || !std::isfinite(c.taker_fee) || c.taker_fee < 0 ||
        c.taker_fee > 1000 || !std::isfinite(c.residual_penalty) || c.residual_penalty < 0)
        throw std::invalid_argument("Invalid execution configuration");
}
}  // namespace
std::string_view policy_name(Policy p) {
    switch (p) {
        case Policy::Aggressive:
            return "aggressive";
        case Policy::Twap:
            return "twap";
        case Policy::Pov:
            return "pov";
        case Policy::Passive:
            return "passive";
        case Policy::Adaptive:
            return "adaptive";
    }
    throw std::invalid_argument("Unknown execution policy");
}
Decision decide(Policy p, const Observation& o, const ExecutionConfig& c) {
    validate(c);
    if (o.time < 0 || o.time > c.horizon || o.filled < 0 || o.filled > c.quantity ||
        o.external_volume < 0)
        throw std::invalid_argument("Invalid policy observation");
    const auto remaining = c.quantity - o.filled;
    if (remaining == 0) return {};
    // At the deadline every policy cancels and attempts the same market closeout.
    if (o.time == c.horizon || p == Policy::Aggressive) return {remaining, true, 0};
    const double fraction = static_cast<double>(std::min(c.horizon, o.time + c.interval)) /
                            static_cast<double>(c.horizon);
    const auto target =
        static_cast<Quantity>(std::floor(static_cast<double>(c.quantity) * fraction));
    const auto due = std::clamp(target - o.filled, Quantity{0}, remaining);
    if (p == Policy::Twap) return {due, true, 0};
    if (p == Policy::Pov) {
        const double target_volume =
            std::min(static_cast<double>(c.quantity),
                     std::floor(static_cast<double>(o.external_volume) * c.participation));
        return {std::clamp(static_cast<Quantity>(target_volume) - o.filled, Quantity{0}, remaining),
                true, 0};
    }
    const auto& touch = c.side == Side::Buy ? o.top.bid : o.top.ask;
    if (!touch) return {};
    if (p == Policy::Passive) return {remaining, false, touch->price};
    if (p == Policy::Adaptive) {
        const auto spread = o.top.bid && o.top.ask ? o.top.ask->price - o.top.bid->price : kNoPrice;
        const bool catch_up = due > c.quantity / 5;
        return {due, spread <= 2 || catch_up, touch->price};
    }
    throw std::invalid_argument("Unknown execution policy");
}
MarketTape generate_tape(const ModelParams& params, std::uint64_t seed, Timestamp horizon) {
    SimConfig c;
    c.params = params;
    c.seed = seed;
    c.horizon = horizon;
    c.record_events = true;
    MarketSimulator sim(c);
    sim.run();
    return {params, horizon, seed, sim.events()};
}
ExecutionResult replay_execution(const MarketTape& tape, Policy policy, const ExecutionConfig& c) {
    validate(c);
    (void)policy_name(policy);
    if (!tape.params.is_valid() || tape.horizon < c.horizon)
        throw std::invalid_argument("Tape/config mismatch");
    Timestamp previous = 0;
    for (const auto& e : tape.events) {
        if (e.t < previous || e.t > tape.horizon || e.order_id >= algo_base || e.quantity <= 0 ||
            e.filled < 0 || e.filled > e.quantity || static_cast<int>(e.type) > 5)
            throw std::invalid_argument("Malformed market tape");
        previous = e.t;
    }
    OrderBook book;
    OrderId id = 1;
    for (int level = tape.params.initial_depth_levels - 1; level >= 0; --level) {
        book.submit_limit(Side::Buy, -tape.params.initial_half_spread - level,
                          tape.params.initial_level_size, 1, id++);
        book.submit_limit(Side::Sell, tape.params.initial_half_spread + level,
                          tape.params.initial_level_size, 1, id++);
    }
    ExecutionResult result;
    result.requested = c.quantity;
    result.arrival_mid = *book.mid_price();
    OrderId next_algo = algo_base, passive_id = 0;
    Quantity external_volume = 0;
    auto account = [&](const SubmitResult& submitted) {
        for (const auto& t : submitted.trades) {
            if (t.aggressor >= algo_base || t.passive >= algo_base) {
                const bool passive = t.passive >= algo_base;
                result.fills.push_back({t.time, t.quantity, t.price, passive});
                result.fee_notional +=
                    static_cast<double>(t.quantity) * (passive ? c.maker_fee : c.taker_fee);
                result.filled += t.quantity;
                result.fill_notional +=
                    static_cast<double>(t.quantity) * static_cast<double>(t.price);
            }
        }
        if (result.filled > c.quantity) throw std::logic_error("Execution overfill");
    };
    auto arrive = [&](std::size_t index, Timestamp time) {
        auto& action = result.actions[index];
        action.arrival_time = time;
        action.arrived_quantity = std::min(action.decision.quantity, c.quantity - result.filled);
        if (action.arrived_quantity > 0) {
            const auto order_id = next_algo++;
            if (action.decision.market)
                account(book.submit_market(c.side, action.arrived_quantity, 2, order_id, time));
            else {
                account(book.submit_limit(c.side, action.decision.price, action.arrived_quantity,
                                          2, order_id, time));
                passive_id = order_id;
            }
        }
    };
    std::size_t cursor = 0;
    std::optional<std::pair<std::size_t, Timestamp>> pending;
    Timestamp next_decision = 0;
    for (Timestamp time = 0;;) {
        // External events win equal timestamps over both arrivals and decisions.
        while (cursor < tape.events.size() && tape.events[cursor].t <= time) {
            const auto& e = tape.events[cursor++];
            const Side side = (e.type == EventType::LimitBid || e.type == EventType::CancelBid ||
                               e.type == EventType::MarketBuy)
                                  ? Side::Buy
                                  : Side::Sell;
            if (e.type == EventType::CancelBid || e.type == EventType::CancelAsk)
                book.cancel(e.order_id);
            else if (e.type == EventType::MarketBuy || e.type == EventType::MarketSell) {
                account(book.submit_market(side, e.quantity, 1, e.order_id, e.t));
                external_volume += e.filled;  // shared observed tape volume, never future volume
            } else
                account(book.submit_limit(side, e.price, e.quantity, 1, e.order_id, e.t));
        }
        if (time == c.horizon) pending.reset();  // hard deadline preempts late arrivals
        if (pending && pending->second == time) {
            arrive(pending->first, time);
            pending.reset();
        }
        if (time == next_decision || time == c.horizon) {
            // One submission in flight: do not accumulate stale budget decisions.
            if (!pending) {
                if (passive_id) {
                    book.cancel(passive_id);
                    passive_id = 0;
                }
                const auto decision =
                    decide(policy, {time, result.filled, external_volume, book.top_of_book()}, c);
                result.actions.push_back({time, decision, result.filled});
                const auto index = result.actions.size() - 1;
                if (time == c.horizon || c.submission_latency == 0)
                    arrive(index, time);
                else
                    pending = {{index, time + c.submission_latency}};
            }
            next_decision = std::min(c.horizon, next_decision + c.interval);
        }
        book.check_invariants();
        if (time == c.horizon) break;
        time = next_decision;
        if (pending) time = std::min(time, pending->second);
    }
    result.residual = c.quantity - result.filled;
    result.fill_rate = static_cast<double>(result.filled) / static_cast<double>(c.quantity);
    const double sign = c.side == Side::Buy ? 1.0 : -1.0;
    // Residual is marked, not reported as a fill; fallback is the arrival mid
    // if a one-sided book prevents observing a valid final mid.
    result.final_mid_available = book.mid_price().has_value();
    const double mark = book.mid_price().value_or(result.arrival_mid);
    result.final_mark = mark;
    result.cost_ticks =
        (sign * (result.fill_notional + mark * static_cast<double>(result.residual) -
                 result.arrival_mid * static_cast<double>(c.quantity)) +
         c.residual_penalty * static_cast<double>(result.residual) + result.fee_notional) /
        static_cast<double>(c.quantity);
    return result;
}
}  // namespace mercury
