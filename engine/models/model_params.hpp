// Mercury — market-model parameters.
//
// This struct is the *input* to the simulation: it is the point in parameter
// space that the order-flow model is evaluated at, and the object the
// adversarial search (Phase 7) will move around. Two rules govern it:
//
//   1. Every field has a documented meaning and a **plausibility box** — an
//      interval of values we are willing to call a market. Search is confined
//      to these boxes so that "breaking" an algorithm by inventing a market that
//      cannot happen is not a result (plan section 2).
//   2. The defaults are a single, named baseline ("the reference market") that
//      the whole project quotes results against. It is a modelling choice, not
//      a measurement, and is labelled as such.
//
// All rates are expressed **per second of virtual time** so that they are
// independent of the simulator's step size.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>

#include "engine/core/types.hpp"

namespace mercury {

/// Parameters describing the stochastic order-flow environment.
///
/// The model is queue-reactive: the arrival intensity of each event type
/// depends on the current book state (chiefly queue imbalance and the spread).
/// The fields below set the *baseline* intensities and the strength of the
/// state dependence.
struct ModelParams {
    // -- Event baseline intensities (events per second) ---------------------
    //
    // These are the intensities at the *reference* state (balanced queues,
    // spread at one tick). The simulator multiplies them by a state-dependent
    // factor derived from the fields further down.

    /// Baseline rate at which new limit orders arrive at or inside the touch.
    double limit_rate{8.0};

    /// Baseline rate at which resting orders are cancelled.
    double cancel_rate{6.0};

    /// Baseline rate at which market (liquidity-taking) orders arrive.
    double market_rate{2.5};

    // -- State dependence ---------------------------------------------------

    /// How strongly the *cancel* intensity responds to queue imbalance.
    /// Positive means: cancellations cluster on the side with the smaller queue
    /// (the side that is about to be lifted), which is the empirically observed
    /// "queue depletion before a price move" effect. A value of 0 makes
    /// cancellation state-independent.
    double cancel_imbalance_sensitivity{1.2};

    /// How strongly *market* (liquidity-taking) intensity responds to queue
    /// imbalance. **Signed**: the sign selects the direction of the response,
    /// and the plausibility box below allows both signs.
    ///
    ///   * negative = contrarian: takers *lean against* the queue that grew,
    ///     i.e. they sell into a deep bid and buy into a deep ask. This is the
    ///     queue-reactive structure of Huang-Lehalle-Rosenbaum and the
    ///     reference-market default: it is what makes aggressive flow
    ///     mean-revert the imbalance instead of amplifying it.
    ///   * positive = momentum: takers *lean with* the queue, lifting the thin
    ///     side and amplifying the imbalance. The search (Phase 7) moves
    ///     through zero to reach these regimes.
    ///   * 0 = market flow is a state-independent Poisson process.
    ///
    /// Concretely, with `imb = (bid_depth - ask_depth) / total_depth` the buy
    /// and sell multipliers are `1 + s*imb` and `1 - s*imb` respectively.
    ///
    /// The *total* market intensity grows with |imbalance| regardless of sign
    /// (a lopsided book invites more liquidity-taking); the sign only steers
    /// the buy/sell split. Implemented per side in market_simulator.cpp.
    double market_imbalance_sensitivity{-0.8};

    /// How strongly the *limit* intensity responds to the spread: a wider
    /// spread attracts more passive liquidity (market-making incentive).
    /// Multiplies the limit rate by (1 + sensitivity * (spread - 1)).
    double limit_spread_sensitivity{0.15};

    // -- Order-flow asymmetry and impact ------------------------------------

    /// Signed lean of the trade flow. 0 = symmetric; +1 means buy-side flow
    /// dominates and drifts the price up. This is the main knob the search uses
    /// to create *directionally adverse* regimes for an executing strategy.
    double flow_imbalance{0.0};

    /// Per-event price-impact scale: the typical number of ticks the mid moves
    /// in response to a unit of net order flow, before clamping to the model's
    /// tick grid. Larger values make the environment more volatile.
    double impact_ticks_per_unit{0.05};

    // -- Self-excitation (Hawkes) -------------------------------------------

    /// Strength of exponential self-excitation on event arrivals, in [0, 0.99).
    /// 0 disables Hawkes behaviour (pure Poisson). Higher values cluster events
    /// in time, reproducing volatility clustering.
    double hawkes_alpha{0.0};

    /// Decay rate of the Hawkes kernel (per second). Larger = faster decay, so
    /// clustering is shorter-lived. Only meaningful when hawkes_alpha > 0.
    double hawkes_beta{5.0};

    // -- Size distribution --------------------------------------------------

    /// Mean size of a limit order, in shares.
    double mean_limit_size{200.0};

    /// Mean size of a market order, in shares.
    double mean_market_size{120.0};

    // -- Environment geometry ------------------------------------------------

    /// Number of price levels each side is seeded with when the book starts.
    int initial_depth_levels{10};

    /// Shares resting at each seeded level.
    Quantity initial_level_size{500};

    /// Half-spread (in ticks) between best bid and best ask at t = 0. The
    /// initial mid is placed at 0 so bid[0] = -half_spread, ask[0] = +half_spread.
    Price initial_half_spread{1};

    /// Tick size in units of the notional price (used only for reporting; the
    /// engine works in ticks).
    double tick_size{0.01};

    // -- Derived helpers -----------------------------------------------------

    /// Total baseline event intensity per second (used to size the next-event
    /// time). Not a probability; just the sum of the baseline rates.
    [[nodiscard]] double total_baseline_rate() const noexcept {
        return limit_rate + cancel_rate + market_rate;
    }

    /// True when the parameters satisfy the model's structural constraints.
    /// The simulator will refuse to run otherwise rather than silently
    /// producing nonsense (for example a negative rate or an unstable Hawkes
    /// kernel with alpha >= beta).
    [[nodiscard]] bool is_valid() const noexcept {
        const double values[] = {limit_rate, cancel_rate, market_rate,
            cancel_imbalance_sensitivity, market_imbalance_sensitivity,
            limit_spread_sensitivity, flow_imbalance, impact_ticks_per_unit,
            hawkes_alpha, hawkes_beta, mean_limit_size, mean_market_size, tick_size};
        for (double value : values) if (!std::isfinite(value)) return false;
        if (cancel_imbalance_sensitivity < 0 || cancel_imbalance_sensitivity > 3 ||
            std::fabs(market_imbalance_sensitivity) > 3 || limit_spread_sensitivity < 0 ||
            limit_spread_sensitivity > 1 || tick_size <= 0) return false;
        // Resource bounds also prevent float-to-integer overflow in event generation.
        if (limit_rate > 10000 || cancel_rate > 10000 || market_rate > 10000 ||
            mean_limit_size > 1000000 || mean_market_size > 1000000 ||
            initial_depth_levels > 1000 || initial_level_size > 1000000000 ||
            initial_half_spread > 1000000000 || hawkes_beta > 1000000) return false;
        if (limit_rate < 0.0 || cancel_rate < 0.0 || market_rate < 0.0) return false;
        if (limit_rate + cancel_rate + market_rate <= 0.0) return false;
        if (mean_limit_size <= 0.0 || mean_market_size <= 0.0) return false;
        if (initial_depth_levels < 1 || initial_level_size <= 0) return false;
        if (initial_half_spread < 1) return false;
        if (hawkes_alpha < 0.0 || hawkes_alpha >= 1.0) return false;
        if (hawkes_beta <= 0.0) return false;
        if (impact_ticks_per_unit < 0.0) return false;
        if (flow_imbalance < -1.0 || flow_imbalance > 1.0) return false;
        return true;
    }
};

/// Inclusive interval used as a search bound for one parameter.
struct ParamBox {
    double lo;
    double hi;
};

/// A plausibility box for the whole parameter space. Search samples only inside
/// these intervals. The boxes are deliberately wide but not absurd; see plan
/// section 4. They are a *modelling judgement* and will be narrowed by
/// calibration in Phase 6.
struct ParamSpace {
    // Order matters: indices into `boxes` are stable and used by the search.
    enum Index : std::size_t {
        LimitRate = 0,
        CancelRate,
        MarketRate,
        CancelImbalance,
        MarketImbalance,
        LimitSpread,
        FlowImbalance,
        ImpactScale,
        HawkesAlpha,
        HawkesBeta,
        MeanLimitSize,
        MeanMarketSize,
        Count
    };

    static constexpr std::size_t kDimension = Count;

    std::array<ParamBox, kDimension> boxes{{
        {0.5, 40.0},    // LimitRate
        {0.2, 30.0},    // CancelRate
        {0.1, 20.0},    // MarketRate
        {0.0, 3.0},     // CancelImbalance
        {-3.0, 3.0},    // MarketImbalance
        {0.0, 1.0},     // LimitSpread
        {-0.9, 0.9},    // FlowImbalance
        {0.0, 0.5},     // ImpactScale
        {0.0, 0.95},    // HawkesAlpha
        {0.5, 20.0},    // HawkesBeta
        {20.0, 2000.0}, // MeanLimitSize
        {10.0, 1200.0}  // MeanMarketSize
    }};

    /// Extract a parameter value by index from a `ModelParams`.
    [[nodiscard]] static double get(const ModelParams& p, std::size_t index) {
        switch (index) {
            case LimitRate: return p.limit_rate;
            case CancelRate: return p.cancel_rate;
            case MarketRate: return p.market_rate;
            case CancelImbalance: return p.cancel_imbalance_sensitivity;
            case MarketImbalance: return p.market_imbalance_sensitivity;
            case LimitSpread: return p.limit_spread_sensitivity;
            case FlowImbalance: return p.flow_imbalance;
            case ImpactScale: return p.impact_ticks_per_unit;
            case HawkesAlpha: return p.hawkes_alpha;
            case HawkesBeta: return p.hawkes_beta;
            case MeanLimitSize: return p.mean_limit_size;
            case MeanMarketSize: return p.mean_market_size;
            default: return 0.0;
        }
    }

    /// Set a parameter value by index on a `ModelParams`.
    static void set(ModelParams& p, std::size_t index, double value) {
        switch (index) {
            case LimitRate: p.limit_rate = value; break;
            case CancelRate: p.cancel_rate = value; break;
            case MarketRate: p.market_rate = value; break;
            case CancelImbalance: p.cancel_imbalance_sensitivity = value; break;
            case MarketImbalance: p.market_imbalance_sensitivity = value; break;
            case LimitSpread: p.limit_spread_sensitivity = value; break;
            case FlowImbalance: p.flow_imbalance = value; break;
            case ImpactScale: p.impact_ticks_per_unit = value; break;
            case HawkesAlpha: p.hawkes_alpha = value; break;
            case HawkesBeta: p.hawkes_beta = value; break;
            case MeanLimitSize: p.mean_limit_size = value; break;
            case MeanMarketSize: p.mean_market_size = value; break;
            default: break;
        }
    }

    /// Human-readable name for a parameter index (for logs and figures).
    [[nodiscard]] static const char* name(std::size_t index) {
        switch (index) {
            case LimitRate: return "limit_rate";
            case CancelRate: return "cancel_rate";
            case MarketRate: return "market_rate";
            case CancelImbalance: return "cancel_imbalance";
            case MarketImbalance: return "market_imbalance";
            case LimitSpread: return "limit_spread";
            case FlowImbalance: return "flow_imbalance";
            case ImpactScale: return "impact_scale";
            case HawkesAlpha: return "hawkes_alpha";
            case HawkesBeta: return "hawkes_beta";
            case MeanLimitSize: return "mean_limit_size";
            case MeanMarketSize: return "mean_market_size";
            default: return "?";
        }
    }
};

/// The named baseline market used throughout the project. A modelling choice,
/// not a measurement.
[[nodiscard]] inline ModelParams reference_market() noexcept {
    return ModelParams{};
}

}  // namespace mercury
