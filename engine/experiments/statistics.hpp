#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
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
}  // namespace mercury
