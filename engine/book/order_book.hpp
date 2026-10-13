// Mercury — price-time-priority limit order book.
//
// Design notes
// ------------
// The book is deliberately small and explicit. It is the component every other
// part of Mercury (simulator, execution policies, adversarial search) depends
// on, so its invariants are the ones we can afford to be pedantic about:
//
//   * A single `std::map` per side holds aggregate volume at each price level.
//     `std::map` keeps prices sorted, so `begin()` is the best level in O(1).
//   * Within a level, resting orders form an intrusive FIFO linked list. This
//     gives O(1) insertion at the tail and O(1) removal given a handle, which
//     is what a cancel needs. We never scan a level to find an order.
//   * Every order lives in an order-id-indexed table. A cancel or an amend is a
//     single hash lookup plus list surgery, never a search.
//
// Matching is strict price-time priority: better price first, and among equal
// prices the order that arrived first. Crossing the book (best_bid >= best_ask)
// is an invariant violation and is only ever produced *during* a match, never
// left behind: `match_against` walks levels until the incoming order is either
// filled or no longer marketable.
//
// The book does **not** know about time. Timestamps are attached to orders by
// the caller (the simulator) so that replay is a pure function of the event
// stream. This keeps the matching path free of any clock reads.
//
// Determinism: iteration order is fully determined by price and insertion
// order, and there is no hashing on any path that feeds matching decisions
// (the order-id map is used only for lookups by id, never for iteration over
// candidates). Two runs on the same event stream produce byte-identical books.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/core/types.hpp"

namespace mercury {

/// A resting (or just-arrived) order. `prev`/`next` are owned by the book's
/// per-level FIFO; callers treat this struct as read-only.
struct Order {
    OrderId id{kInvalidOrderId};
    ParticipantId participant{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Price price{kNoPrice};              ///< limit price; kNoPrice for Market
    Quantity quantity{0};               ///< original quantity
    Quantity remaining{0};              ///< shares still unfilled
    Timestamp arrival{0};               ///< simulator time the order was accepted
    bool in_book{false};                ///< false once filled/cancelled/never rested
};

/// Outcome of a single submission, reported back to the caller for accounting.
struct SubmitResult {
    OrderId id{kInvalidOrderId};
    Quantity filled{0};                 ///< total shares matched on arrival
    Quantity resting{0};                ///< shares left in the book (0 for Market/IOC)
    OrderStatus status{OrderStatus::Rejected};
    Price last_trade_price{kInvalidPrice};
};

/// One executed match. Emitted in price-time order as a submission walks the
/// opposite side of the book.
struct Trade {
    OrderId aggressor{kInvalidOrderId}; ///< the incoming order
    OrderId passive{kInvalidOrderId};   ///< the resting order that was hit
    Side aggressor_side{Side::Buy};
    Price price{kInvalidPrice};
    Quantity quantity{0};
    Timestamp time{0};
};

/// A single aggregate price level, as reported by top-of-book helpers.
struct Level {
    Price price{kInvalidPrice};
    Quantity quantity{0};
    std::size_t order_count{0};
};

/// A top-of-book snapshot. `bid`/`ask` are `nullopt` on an empty side.
struct TopOfBook {
    std::optional<Level> bid;
    std::optional<Level> ask;
};

class OrderBook {
public:
    OrderBook() = default;

    // Non-copyable (the order index holds iterators into the level lists), but
    // movable: moving is safe because std::map/std::list moves preserve
    // element addresses, so the stored iterators remain valid.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    // -- Mutation ---------------------------------------------------------

    /// Submit a limit order. If it crosses the opposite side it matches
    /// immediately at resting prices (price improvement goes to the aggressor),
    /// and any remainder rests if the TIF permits (GTC rests, IOC and FOK do
    /// not). Returns the outcome. `id == 0` allocates a fresh id.
    SubmitResult submit_limit(Side side, Price price, Quantity quantity,
                              ParticipantId participant = 0, OrderId id = 0,
                              Timestamp time = 0,
                              TimeInForce tif = TimeInForce::GTC);

    /// Submit a market order. It never rests; any unfilled remainder is
    /// cancelled and reported via `status`.
    SubmitResult submit_market(Side side, Quantity quantity,
                               ParticipantId participant = 0, OrderId id = 0,
                               Timestamp time = 0);

    /// Cancel an order by id. Returns true if the order was resting and has now
    /// been removed; false if the id is unknown or no longer resting.
    bool cancel(OrderId id);

    /// Reduce the remaining quantity of a resting order in place, keeping its
    /// time priority. `new_quantity` must be strictly between 0 and the current
    /// remaining; a request to zero is a cancel. Returns false if not possible.
    bool reduce(OrderId id, Quantity new_quantity);

    // -- Queries ----------------------------------------------------------

    [[nodiscard]] const Order* find(OrderId id) const;

    /// Best price on a side, or nullopt if that side is empty.
    [[nodiscard]] std::optional<Price> best_price(Side side) const;

    /// Aggregate volume available at a specific price (0 if no such level).
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;

    [[nodiscard]] TopOfBook top_of_book() const;

    /// Mid price of the top of book, or nullopt if either side is empty.
    [[nodiscard]] std::optional<double> mid_price() const;

    /// Absolute bid-ask spread in ticks, or nullopt if either side is empty.
    [[nodiscard]] std::optional<Price> spread() const;

    [[nodiscard]] bool empty() const noexcept { return order_index_.empty(); }
    [[nodiscard]] std::size_t resting_order_count() const noexcept {
        return order_index_.size();
    }
    [[nodiscard]] std::size_t level_count(Side side) const;
    [[nodiscard]] Quantity total_quantity(Side side) const;

    /// Sum of resting quantity on a side for prices at least as aggressive as
    /// `price`. Used by execution policies to size against visible depth.
    [[nodiscard]] Quantity depth_at_least(Side side, Price price) const;

    /// Depth profile up to `levels` levels from the touch, in price order.
    [[nodiscard]] std::vector<Level> levels(Side side, std::size_t max_levels) const;

    /// Debug/QA helper: checks the book's internal invariants (aggregates match
    /// the underlying orders, best_bid < best_ask, linked lists are consistent,
    /// index and lists agree). Throws std::logic_error on the first violation.
    void check_invariants() const;

private:
    // A price level: aggregate quantity plus a FIFO of resting orders.
    struct LevelState {
        Quantity total_quantity{0};
        std::list<Order> orders;  // front = oldest = highest time priority
    };

    // An order's position in its level's FIFO, so removal is O(1).
    struct OrderLocator {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Best price with a non-zero level, if any.
    [[nodiscard]] std::optional<Price> best_price_unlocked(Side side) const;

    // Walk the opposite side, matching `aggressor` until it is filled or no
    // longer marketable. Appends executed trades to `trades`.
    void match_against(Order& aggressor, std::vector<Trade>* trades);

    // Insert a fully-validated order into the book as a resting order.
    void rest(Order order);

    // Remove a resting order, cleaning up its level if it becomes empty.
    void remove_resting(OrderId id);

    std::map<Price, LevelState, std::greater<Price>> bids_;
    std::map<Price, LevelState, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderLocator> order_index_;
    OrderId next_auto_id_{1};
};

}  // namespace mercury
