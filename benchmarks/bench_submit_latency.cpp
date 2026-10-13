// Mercury — order-book submit/cancel latency benchmark.
//
// Hypothesis tested: the book's O(log L) level lookup plus O(1) FIFO
// insertion/removal keeps per-operation cost roughly flat as the number of
// resting orders grows, so latency is dominated by constant factors rather
// than by a linear scan over the level.
//
// We measure, not assert: the numbers printed here are for this machine only
// and are not claimed to transfer to any other. See docs/implementation_plan.md
// for the standing rule that no unmeasured number is reported.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "engine/book/order_book.hpp"

using namespace mercury;
using Clock = std::chrono::steady_clock;

namespace {

double percentile(std::vector<double>& xs, double p) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    const auto n = static_cast<double>(xs.size() - 1);
    const auto idx = static_cast<std::size_t>(p * n);
    return xs[idx];
}

void report(const char* name, double ns_per_op, double p50, double p99) {
    std::cout << "bench," << name << ",mean_ns_per_op," << ns_per_op << ",ns\n";
    std::cout << "bench," << name << ",p50_ns," << p50 << ",ns\n";
    std::cout << "bench," << name << ",p99_ns," << p99 << ",ns\n";
}

}  // namespace

int main() {
    std::mt19937_64 rng(20241011u);
    std::uniform_int_distribution<Price> price_dist(10000, 10100);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    for (int target_orders : {1000, 10000, 100000}) {
        OrderBook book;
        // Fill the book with non-crossing limit orders on both sides.
        for (int i = 0; i < target_orders; ++i) {
            const bool buy = (i % 2) == 0;
            const Price price = buy ? 10000 - (i % 50) : 10100 + (i % 50);
            book.submit_limit(buy ? Side::Buy : Side::Sell, price, qty_dist(rng),
                              0, static_cast<OrderId>(i + 1));
        }

        const int iterations = 200000;
        std::vector<double> submit_ns;
        std::vector<double> cancel_ns;
        submit_ns.reserve(iterations);
        cancel_ns.reserve(iterations);

        // Submit/cancel pairs at prices that do not cross, so the book stays
        // stable in size across the run.
        OrderId id = static_cast<OrderId>(target_orders + 1);
        for (int i = 0; i < iterations; ++i) {
            const bool buy = (i % 2) == 0;
            const Price price = buy ? 10000 - (i % 50) : 10100 + (i % 50);

            auto t0 = Clock::now();
            book.submit_limit(buy ? Side::Buy : Side::Sell, price, qty_dist(rng),
                              0, id);
            auto t1 = Clock::now();
            book.cancel(id);
            auto t2 = Clock::now();

            submit_ns.push_back(
                std::chrono::duration<double, std::nano>(t1 - t0).count());
            cancel_ns.push_back(
                std::chrono::duration<double, std::nano>(t2 - t1).count());
            ++id;
        }

        const auto mean = [](const std::vector<double>& xs) {
            double s = 0;
            for (double x : xs) s += x;
            return xs.empty() ? 0.0 : s / static_cast<double>(xs.size());
        };

        const std::string suffix = "n" + std::to_string(target_orders);
        report(("submit_" + suffix).c_str(), mean(submit_ns),
               percentile(submit_ns, 0.50), percentile(submit_ns, 0.99));
        report(("cancel_" + suffix).c_str(), mean(cancel_ns),
               percentile(cancel_ns, 0.50), percentile(cancel_ns, 0.99));
    }
    return 0;
}
