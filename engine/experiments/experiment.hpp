#pragma once
#include <string>
#include <vector>

#include "engine/execution/execution.hpp"
namespace mercury {
struct Trial {
    std::uint64_t seed{};
    Policy policy{};
    ExecutionResult result;
    std::vector<SimEvent> external_events;  // first seed/first policy only, for audit exports
};
std::vector<Trial> compare_policies(const ModelParams& params, const ExecutionConfig& config,
                                    std::uint64_t first_seed, std::size_t runs,
                                    std::size_t workers);
struct SearchTrial {
    ModelParams params;
    double score{};
};
struct SearchResult {
    ModelParams selected;
    std::vector<SearchTrial> trials;
};
// Synthetic bounds, not calibrated plausibility bounds. Optimizes TWAP CVaR95.
SearchResult search_regimes(const ExecutionConfig& config, std::uint64_t seed,
                            std::size_t candidates, std::size_t training_runs);
}  // namespace mercury
