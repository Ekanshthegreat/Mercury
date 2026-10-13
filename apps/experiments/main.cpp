#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#include "engine/experiments/experiment.hpp"
#include "engine/experiments/statistics.hpp"
namespace {
using namespace mercury;
struct Options {
    std::uint64_t seed{7};
    std::size_t runs{32}, candidates{16}, training{16}, workers{2};
    std::string output{"results/demo"};
    ExecutionConfig execution;
};
std::uint64_t integer(const std::string& text, std::uint64_t lo, std::uint64_t hi) {
    std::uint64_t n{};
    const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || n < lo || n > hi)
        throw std::invalid_argument("Invalid numeric option: " + text);
    return n;
}
void usage() {
    std::cout
        << "Mercury experiment runner " << MERCURY_VERSION
        << "\n"
           "mercury_experiment compare|stress [options]\n"
           "  --out DIRECTORY  --runs 2..1000  --seed 0..1000000000\n"
           "  --seconds 1..300 --quantity 1..1000000 --workers 1..32\n"
           "  --candidates 1..100 --training-runs 2..256 (stress only)\n"
           "  --side buy|sell\n"
           "Outputs run/summary CSV, full configuration JSON and decision traces.\n"
           "Stress optimizes TWAP CVaR95 on training seeds, evaluates separate holdout seeds.\n";
}
std::ofstream file(const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path.string());
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(17);
    return out;
}
void params_json(std::ostream& s, const ModelParams& p) {
    s << "{\"limit_rate\":" << p.limit_rate << ",\"cancel_rate\":" << p.cancel_rate
      << ",\"market_rate\":" << p.market_rate << ",\"flow_imbalance\":" << p.flow_imbalance
      << ",\"initial_level_size\":" << p.initial_level_size
      << ",\"mean_market_size\":" << p.mean_market_size
      << ",\"cancel_imbalance_sensitivity\":" << p.cancel_imbalance_sensitivity
      << ",\"market_imbalance_sensitivity\":" << p.market_imbalance_sensitivity
      << ",\"limit_spread_sensitivity\":" << p.limit_spread_sensitivity
      << ",\"impact_ticks_per_unit\":" << p.impact_ticks_per_unit
      << ",\"hawkes_alpha\":" << p.hawkes_alpha << ",\"hawkes_beta\":" << p.hawkes_beta
      << ",\"mean_limit_size\":" << p.mean_limit_size
      << ",\"initial_depth_levels\":" << p.initial_depth_levels
      << ",\"initial_half_spread\":" << p.initial_half_spread << ",\"tick_size\":" << p.tick_size
      << "}";
}
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            usage();
            return argc < 2 ? 2 : 0;
        }
        const std::string mode = argv[1];
        if (mode != "compare" && mode != "stress")
            throw std::invalid_argument("Expected compare or stress");
        Options o;
        for (int i = 2; i < argc; ++i) {
            const std::string key = argv[i];
            if (i + 1 == argc) throw std::invalid_argument("Missing value for " + key);
            const std::string value = argv[++i];
            if (key == "--out")
                o.output = value;
            else if (key == "--runs")
                o.runs = integer(value, 2, 1000);
            else if (key == "--seed")
                o.seed = integer(value, 0, 1000000000);
            else if (key == "--seconds")
                o.execution.horizon =
                    static_cast<Timestamp>(integer(value, 1, 300)) * 1'000'000'000;
            else if (key == "--quantity")
                o.execution.quantity = static_cast<Quantity>(integer(value, 1, 1000000));
            else if (key == "--workers")
                o.workers = integer(value, 1, 32);
            else if (key == "--candidates" && mode == "stress")
                o.candidates = integer(value, 1, 100);
            else if (key == "--training-runs" && mode == "stress")
                o.training = integer(value, 2, 256);
            else if (key == "--side") {
                if (value != "buy" && value != "sell")
                    throw std::invalid_argument("Side must be buy or sell");
                o.execution.side = value == "buy" ? Side::Buy : Side::Sell;
            } else
                throw std::invalid_argument("Unknown option: " + key);
        }
        const std::filesystem::path root(o.output);
        std::filesystem::create_directories(root);
        std::vector<std::pair<std::string, ModelParams>> regimes{{"baseline", reference_market()}};
        SearchResult search;
        if (mode == "stress") {
            search = search_regimes(o.execution, o.seed, o.candidates, o.training);
            regimes.emplace_back("selected", search.selected);
            for (const double scale : {0.9, 1.1}) {
                auto p = search.selected;
                p.market_rate = std::clamp(p.market_rate * scale, 1.0, 12.0);
                regimes.emplace_back(scale < 1 ? "neighbor_low_market" : "neighbor_high_market", p);
            }
            auto trials = file(root / "search.csv");
            trials << "candidate,training_cvar95,limit_rate,cancel_rate,market_rate,flow_imbalance,"
                      "initial_level_size,mean_market_size\n";
            for (std::size_t i = 0; i < search.trials.size(); ++i) {
                const auto& t = search.trials[i];
                const auto& p = t.params;
                trials << i << ',' << t.score << ',' << p.limit_rate << ',' << p.cancel_rate << ','
                       << p.market_rate << ',' << p.flow_imbalance << ',' << p.initial_level_size
                       << ',' << p.mean_market_size << '\n';
            }
        }
        const auto first_seed = mode == "stress" ? o.seed + 1'000'000 : o.seed;
        auto manifest = file(root / "manifest.json");
        manifest << "{\n\"format_version\":1,\"engine_version\":\"" << MERCURY_VERSION
                 << "\",\"mode\":\"" << mode << "\",\"seed\":" << o.seed
                 << ",\"first_evaluation_seed\":" << first_seed << ",\"runs\":" << o.runs
                 << ",\"workers\":" << o.workers
                 << ",\"candidates\":" << (mode == "stress" ? o.candidates : 0)
                 << ",\"training_runs\":" << (mode == "stress" ? o.training : 0)
                 << ",\"execution\":{\"side\":\""
                 << (o.execution.side == Side::Buy ? "buy" : "sell")
                 << "\",\"quantity\":" << o.execution.quantity
                 << ",\"horizon_ns\":" << o.execution.horizon
                 << ",\"interval_ns\":" << o.execution.interval
                 << ",\"participation\":" << o.execution.participation
                 << ",\"residual_penalty_ticks\":" << o.execution.residual_penalty
                 << "},\n\"regimes\":{\n";
        auto runs = file(root / "runs.csv"), summary = file(root / "summary.csv"),
             traces = file(root / "decisions.csv");
        runs << "regime,seed,policy,requested,filled,residual,fill_rate,fill_notional,arrival_mid,"
                "cost_ticks\n";
        summary << "regime,policy,runs,mean_ticks,standard_error_ticks,p95_ticks,cvar95_ticks,mean_"
                   "fill_rate,paired_mean_vs_twap,paired_se_vs_twap\n";
        traces << "regime,seed,policy,time_ns,filled_before,submitted_quantity,market,price\n";
        for (std::size_t index = 0; index < regimes.size(); ++index) {
            const auto& [name, params] = regimes[index];
            if (index) manifest << ",\n";
            manifest << '"' << name << "\":";
            params_json(manifest, params);
            const auto results =
                compare_policies(params, o.execution, first_seed, o.runs, o.workers);
            std::map<std::string, std::vector<double>> costs, fills, differences;
            for (std::size_t i = 0; i < results.size(); ++i) {
                const auto& t = results[i];
                const auto& r = t.result;
                const std::string policy(policy_name(t.policy));
                runs << name << ',' << t.seed << ',' << policy << ',' << r.requested << ','
                     << r.filled << ',' << r.residual << ',' << r.fill_rate << ','
                     << r.fill_notional << ',' << r.arrival_mid << ',' << r.cost_ticks << '\n';
                costs[policy].push_back(r.cost_ticks);
                fills[policy].push_back(r.fill_rate);
                differences[policy].push_back(r.cost_ticks -
                                              results[(i / 5) * 5 + 1].result.cost_ticks);
                if (t.seed == first_seed)
                    for (const auto& a : r.actions)
                        traces << name << ',' << t.seed << ',' << policy << ',' << a.time << ','
                               << a.filled_before << ',' << a.decision.quantity << ','
                               << a.decision.market << ',' << a.decision.price << '\n';
            }
            for (const auto& [policy, sample] : costs) {
                const auto risk = summarize(sample), paired = summarize(differences[policy]);
                summary << name << ',' << policy << ',' << o.runs << ',' << risk.mean << ','
                        << risk.standard_error << ',' << risk.p95 << ',' << risk.cvar95 << ','
                        << summarize(fills[policy]).mean << ',' << paired.mean << ','
                        << paired.standard_error << '\n';
            }
            std::cout << name << ": " << o.runs << " shared-seed comparisons complete\n";
        }
        manifest << "\n}}\n";
        std::cout << "Results: " << root.string() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Mercury: " << e.what() << '\n';
        return 2;
    }
}
