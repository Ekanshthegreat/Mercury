#include "engine/experiments/experiment.hpp"

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

#include "engine/experiments/statistics.hpp"
namespace mercury {
std::vector<Trial> compare_policies(const ModelParams& params, const ExecutionConfig& config,
                                    std::uint64_t first_seed, std::size_t runs,
                                    std::size_t workers) {
    if (runs < 1 || runs > 10000 || workers < 1 || workers > 32 ||
        first_seed > std::numeric_limits<std::uint64_t>::max() - runs)
        throw std::invalid_argument("Invalid experiment budget");
    const Policy policies[] = {Policy::Aggressive, Policy::Twap, Policy::Pov, Policy::Passive,
                               Policy::Adaptive};
    std::vector<Trial> results(runs * 5);
    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex mutex;
    auto work = [&] {
        try {
            for (;;) {
                const auto i = next.fetch_add(1);
                if (i >= runs) break;
                const auto seed = first_seed + i;
                const auto tape = generate_tape(params, seed, config.horizon);
                for (std::size_t j = 0; j < 5; ++j)
                    results[i * 5 + j] = {seed, policies[j],
                                          replay_execution(tape, policies[j], config)};
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!error) error = std::current_exception();
        }
    };
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < std::min(workers, runs); ++i) threads.emplace_back(work);
    for (auto& thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
    return results;
}
SearchResult search_regimes(const ExecutionConfig& config, std::uint64_t seed,
                            std::size_t candidates, std::size_t training_runs) {
    if (candidates < 1 || candidates > 500 || training_runs < 2 || training_runs > 1000 ||
        seed > std::numeric_limits<std::uint64_t>::max() - training_runs)
        throw std::invalid_argument("Invalid search budget");
    Rng rng(seed ^ 0xA0761D6478BD642FULL);
    SearchResult out;
    double best = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < candidates; ++i) {
        auto p = reference_market();
        auto sample = [&](double lo, double hi) { return lo + (hi - lo) * rng.uniform_double(); };
        p.limit_rate = sample(2, 20);
        p.cancel_rate = sample(2, 16);
        p.market_rate = sample(1, 12);
        p.flow_imbalance = sample(-0.7, 0.7);
        p.initial_level_size = static_cast<Quantity>(sample(200, 801));
        p.mean_market_size = sample(80, 400);
        std::vector<double> costs;
        for (std::size_t j = 0; j < training_runs; ++j) {
            const auto tape = generate_tape(p, seed + j, config.horizon);
            costs.push_back(replay_execution(tape, Policy::Twap, config).cost_ticks);
        }
        const double score = summarize(costs).cvar95;
        out.trials.push_back({p, score});
        if (score > best) {
            best = score;
            out.selected = p;
        }
    }
    return out;
}
}  // namespace mercury
