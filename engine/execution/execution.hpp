#pragma once
#include <string_view>
#include <vector>
#include "engine/simulation/market_simulator.hpp"
namespace mercury {
enum class Policy { Aggressive, Twap, Pov, Passive, Adaptive };
std::string_view policy_name(Policy policy);
struct ExecutionConfig {
    Side side{Side::Buy};
    Quantity quantity{2000};
    Timestamp horizon{60'000'000'000LL};
    Timestamp interval{1'000'000'000LL};
    double participation{0.15};
    double residual_penalty{10.0}; // ticks per unfilled share at deadline
};
struct Observation {
    Timestamp time{};
    Quantity filled{}, external_volume{};
    TopOfBook top;
};
struct Decision { Quantity quantity{}; bool market{true}; Price price{}; };
Decision decide(Policy policy, const Observation& observation, const ExecutionConfig& config);
struct Action { Timestamp time{}; Decision decision; Quantity filled_before{}; };
struct ExecutionResult {
    Quantity requested{}, filled{}, residual{};
    double arrival_mid{}, fill_notional{}, fill_rate{}, cost_ticks{};
    std::vector<Action> actions;
};
struct MarketTape {
    ModelParams params;
    Timestamp horizon{};
    std::uint64_t seed{};
    std::vector<SimEvent> events;
};
MarketTape generate_tape(const ModelParams& params, std::uint64_t seed, Timestamp horizon);
// Tape generation is independent of the policy. Replay changes queue depletion,
// but does not regenerate external event rates/prices in response to execution.
ExecutionResult replay_execution(const MarketTape& tape, Policy policy, const ExecutionConfig& config);
} // namespace mercury
