#include <limits>

#include "engine/experiments/experiment.hpp"
#include "engine/experiments/statistics.hpp"
#include "test_harness.hpp"
using namespace mercury;
MERCURY_TEST(risk_statistics_match_hand_computed_sample) {
    const auto s = summarize({1, 2, 3, 4});
    MERCURY_CHECK_EQ(s.mean, 2.5);
    MERCURY_CHECK_EQ(s.p95, 4.0);
    MERCURY_CHECK_EQ(s.cvar95, 4.0);
    MERCURY_CHECK(std::abs(s.standard_error - std::sqrt(5.0 / 12.0)) < 1e-12);
}
MERCURY_TEST(cvar_boundary_uses_exact_tail_mass) {
    std::vector<double> sample(30, 0.0);
    sample[29] = 9;
    sample[28] = 3;
    MERCURY_CHECK_EQ(summarize(sample).cvar95, 7.0);  // (9+0.5*3)/1.5
    MERCURY_CHECK_THROWS(summarize({}));
    MERCURY_CHECK_THROWS(summarize({std::numeric_limits<double>::infinity()}));
}
MERCURY_TEST(parallel_comparison_preserves_order_and_results) {
    ExecutionConfig c;
    c.horizon = 3'000'000'000;
    auto a = compare_policies(reference_market(), c, 7, 6, 1);
    auto b = compare_policies(reference_market(), c, 7, 6, 3);
    MERCURY_CHECK_EQ(a.size(), 30U);
    for (std::size_t i = 0; i < a.size(); ++i) {
        MERCURY_CHECK_EQ(a[i].seed, b[i].seed);
        MERCURY_CHECK_EQ(a[i].policy, b[i].policy);
        MERCURY_CHECK_EQ(a[i].result.cost_ticks, b[i].result.cost_ticks);
    }
}
MERCURY_TEST(search_reproducible_and_selects_maximum_training_tail) {
    ExecutionConfig c;
    c.horizon = 3'000'000'000;
    const auto a = search_regimes(c, 12, 4, 3), b = search_regimes(c, 12, 4, 3);
    double maximum = -1e100;
    for (std::size_t i = 0; i < a.trials.size(); ++i) {
        MERCURY_CHECK_EQ(a.trials[i].score, b.trials[i].score);
        MERCURY_CHECK(a.trials[i].params.is_valid());
        maximum = std::max(maximum, a.trials[i].score);
    }
    bool found = false;
    for (const auto& trial : a.trials)
        if (trial.params.limit_rate == a.selected.limit_rate && trial.score == maximum)
            found = true;
    MERCURY_CHECK(found);
}
MERCURY_TEST(worker_errors_propagate_and_budgets_are_checked) {
    ExecutionConfig c;
    c.quantity = -1;
    MERCURY_CHECK_THROWS(compare_policies(reference_market(), c, 1, 2, 2));
    MERCURY_CHECK_THROWS(compare_policies(reference_market(), c, 1, 0, 1));
    MERCURY_CHECK_THROWS(search_regimes(c, 1, 0, 2));
}
MERCURY_MAIN()
