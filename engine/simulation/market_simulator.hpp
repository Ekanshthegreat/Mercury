// Mercury — state-dependent order-flow market simulator.
//
// The simulator is a discrete-event generator. It advances a virtual clock and
// injects orders into an `OrderBook`, choosing *what* event happens next and
// *when* according to intensities that depend on the current book state. This
// is the queue-reactive idea (Huang–Lehalle–Rosenbaum): the rates of limit,
// cancel and market events are functions of the book, chiefly the queue
// imbalance and the spread.
//
// Event timing
// ------------
// With baseline rates \f$\lambda_i\f$ and a state-dependent multiplier
// \f$m_i(\text{state})\f$, the next event is drawn from a non-homogeneous
// Poisson process. We use the standard thinning approach:
//
//   1. compute the current total intensity \f$\Lambda = \sum_i \lambda_i m_i\f$
//      (including any Hawkes excitation),
//   2. draw the waiting time \f$\tau \sim \text{Exp}(\Lambda)\f$,
//   3. choose the event type \f$i\f$ with probability \f$\lambda_i m_i / \Lambda\f$.
//
// Because the state changes only at event times, the intensity is piecewise
// constant and the exponential draw between events is exact — no discretisation
// bias. (Hawkes excitation is folded into \f$\Lambda\f$ at the moment of the
// draw, which makes the process a Markov-modulated approximation of a true
// Hawkes process; this is documented as a modelling choice, not hidden.)
//
// Determinism
// -----------
// The simulator holds one `Rng`. Given the same `ModelParams` and seed it
// produces *exactly* the same sequence of events, times, and fills, on any
// platform, because all randomness flows through `Rng` (see util/rng.hpp).
// Nothing here reads wall time.

#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/book/order_book.hpp"
#include "engine/core/types.hpp"
#include "engine/models/model_params.hpp"
#include "engine/util/rng.hpp"

namespace mercury {

/// The kinds of event the simulator can generate. Kept explicit (rather than a
/// generic "order") because the intensities — and therefore the model — are
/// defined per event type.
enum class EventType : std::uint8_t {
    LimitBid,   ///< a new buy limit order
    LimitAsk,   ///< a new sell limit order
    CancelBid,  ///< cancel a resting buy order
    CancelAsk,  ///< cancel a resting sell order
    MarketBuy,  ///< an aggressive buy (market order)
    MarketSell, ///< an aggressive sell (market order)
};

/// One simulated event, recorded so runs can be inspected, replayed, and
/// compared. `t` is virtual nanoseconds; `mid_before`/`mid_after` bracket the
/// effect of the event on the mid price.
struct SimEvent {
    Timestamp t{0};
    EventType type{EventType::LimitBid};
    OrderId order_id{kInvalidOrderId};
    Price price{kNoPrice};
    Quantity quantity{0};
    Quantity filled{0};
    double mid_before{0.0};
    double mid_after{0.0};
};

/// Streaming statistics the simulator maintains over a run. Cheap to update,
/// and enough to assert "rates respond to params" without post-processing.
struct SimStats {
    Quantity limit_orders{0};
    Quantity cancel_orders{0};
    Quantity market_orders{0};
    Quantity shares_traded{0};
    Quantity shares_cancelled{0};
    std::uint64_t event_count{0};

    /// Time-weighted sum of squares of mid-price changes, and count of changes,
    /// used to derive realised volatility in `realised_volatility()`.
    double sum_squared_mid_change{0.0};
    std::uint64_t mid_change_count{0};

    /// Mean realised volatility of the mid, in ticks per sqrt(event). Requires
    /// at least two mid changes to be meaningful; returns 0 otherwise.
    [[nodiscard]] double realised_volatility() const noexcept {
        if (mid_change_count == 0) return 0.0;
        return std::sqrt(sum_squared_mid_change /
                         static_cast<double>(mid_change_count));
    }
};

/// Configuration for a single simulation run.
struct SimConfig {
    ModelParams params{};
    std::uint64_t seed{0};
    Timestamp horizon{0};        ///< virtual nanoseconds to simulate
    bool record_events{false};   ///< keep the full event log (memory heavy)
    ParticipantId participant{1}; ///< tag applied to generated orders
};

/// The market simulator. Owns an `OrderBook` and drives it with generated flow.
class MarketSimulator {
public:
    /// Construct and seed the initial book from `params`.
    explicit MarketSimulator(const SimConfig& config);

    MarketSimulator(const MarketSimulator&) = delete;
    MarketSimulator& operator=(const MarketSimulator&) = delete;
    MarketSimulator(MarketSimulator&&) noexcept = default;
    MarketSimulator& operator=(MarketSimulator&&) noexcept = default;

    /// Run until the horizon is reached. Returns the number of events applied.
    std::uint64_t run();

    /// Advance a single event. Returns false when the horizon is already
    /// reached. Exposed so tests can step deterministically.
    bool step();

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const SimStats& stats() const noexcept { return stats_; }
    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    [[nodiscard]] const std::vector<SimEvent>& events() const noexcept {
        return events_;
    }

    /// The multiplicative factors applied to each baseline intensity at the
    /// current book state. Exposed so tests can assert that the state
    /// dependence actually does something, rather than trusting the code path.
    struct IntensityFactors {
        double limit{1.0};
        double cancel{1.0};
        double market{1.0};
    };
    [[nodiscard]] IntensityFactors current_factors() const;

private:
    /// Build the initial two-sided book from `params`.
    void seed_book();

    /// Signed queue imbalance in [-1, 1]: +1 when bid-side depth dominates,
    /// -1 when ask-side depth dominates, 0 when balanced or empty.
    [[nodiscard]] double imbalance() const;

    /// Current bid-ask spread in ticks (1 if unknown/empty).
    [[nodiscard]] Price current_spread() const;

    /// Multipliers for each event family at the current state.
    [[nodiscard]] IntensityFactors factors() const;

    /// Pick a limit price for a new order on `side`, relative to the touch.
    [[nodiscard]] Price choose_limit_price(Side side);

    /// Random size for a limit order.
    [[nodiscard]] Quantity draw_limit_size();

    /// Random size for a market order.
    [[nodiscard]] Quantity draw_market_size();

    /// Try to cancel a resting order on `side`. Returns the cancelled quantity
    /// (0 if nothing was cancellable).
    [[nodiscard]] Quantity do_cancel(Side side, EventType type);

    /// Submit a limit order, returning the filled quantity.
    Quantity do_limit(Side side, EventType type);

    /// Submit a market order, returning the filled quantity.
    Quantity do_market(Side side, EventType type);

    /// Apply (or decay) Hawkes excitation and return the current extra
    /// intensity per event family.
    [[nodiscard]] double hawkes_boost() const;

    SimConfig config_;
    OrderBook book_;
    Rng rng_;
    Timestamp now_{0};
    SimStats stats_{};
    std::vector<SimEvent> events_;
    OrderId next_order_id_{1};

    // Hawkes state: a single scalar excitation that decays exponentially and is
    // bumped on every event. Exposed via `hawkes_boost()`.
    double hawkes_level_{0.0};
    Timestamp hawkes_last_update_{0};

    // Live resting order ids per side, so cancels have candidates without
    // scanning the book. Kept in sync as orders rest and are removed.
    std::vector<OrderId> live_bids_;
    std::vector<OrderId> live_asks_;
};

}  // namespace mercury
