#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "engine/util/rng.hpp"
namespace mercury {
struct RiskSummary {
    double mean{}, standard_error{}, p95{}, cvar95{};
};
inline RiskSummary summarize(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("Empty risk sample");
    for (double x : values)
        if (!std::isfinite(x)) throw std::invalid_argument("Non-finite risk sample");
    RiskSummary r;
    const double n = static_cast<double>(values.size());
    for (double x : values) r.mean += x / n;
    if (values.size() > 1) {
        double ss = 0;
        for (double x : values) ss += (x - r.mean) * (x - r.mean);
        r.standard_error = std::sqrt(ss / (n - 1) / n);
    }
    std::sort(values.begin(), values.end());
    r.p95 = values[static_cast<std::size_t>(std::ceil(n * 0.95)) - 1];
    // Fractionally weight the boundary order statistic so tail mass is exactly 5%.
    const double mass = n * 0.05;
    double left = mass;
    for (auto i = values.rbegin(); i != values.rend() && left > 0; ++i) {
        const double weight = std::min(left, 1.0);
        r.cvar95 += *i * weight / mass;
        left -= weight;
    }
    return r;
}
// Percentile bootstrap over independent observed run costs. This measures
// empirical sampling uncertainty, not unseen tails or market-model uncertainty.
struct TailInterval {
    double lower{}, upper{};
};
inline TailInterval bootstrap_cvar95(const std::vector<double>& values, std::uint64_t seed,
                                     std::size_t resamples = 512) {
    if (values.size() < 2 || values.size() > 10000 || resamples < 40 || resamples > 5000 ||
        values.size() > 2000000 / resamples)
        throw std::invalid_argument("Invalid tail bootstrap sample or resource budget");
    auto population = values;
    (void)summarize(population);  // Reject non-finite values before drawing.
    // A seed describes the empirical sample, independent of its input ordering.
    std::sort(population.begin(), population.end());
    Rng rng(seed);
    std::vector<double> sample(values.size()), estimates;
    estimates.reserve(resamples);
    for (std::size_t b = 0; b < resamples; ++b) {
        for (auto& x : sample)
            x = population[static_cast<std::size_t>(rng.uniform_u64(population.size()))];
        estimates.push_back(summarize(sample).cvar95);
    }
    std::sort(estimates.begin(), estimates.end());
    const double count = static_cast<double>(resamples);
    return {estimates[static_cast<std::size_t>(std::ceil(count * 0.025)) - 1],
            estimates[static_cast<std::size_t>(std::ceil(count * 0.975)) - 1]};
}
}  // namespace mercury
