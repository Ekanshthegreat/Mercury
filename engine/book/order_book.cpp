// Mercury — price-time-priority limit order book (implementation).

#include "engine/book/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mercury {
namespace {

/// True when `price` on the aggressor's side is marketable against `best`.
/// A buy crosses at any ask <= its limit; a sell crosses at any bid >= limit.
[[nodiscard]] bool crosses(Side aggressor_side, Price limit, Price best) {
    return aggressor_side == Side::Buy ? limit >= best : limit <= best;
}

[[noreturn]] void fail(const std::string& what) {
    throw std::logic_error("OrderBook invariant violated: " + what);
}

}  // namespace

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

SubmitResult OrderBook::submit_limit(Side side, Price price, Quantity quantity,
                                     ParticipantId participant, OrderId id,
                                     Timestamp time, TimeInForce tif)
{
    SubmitResult result;
    result.id = id != kInvalidOrderId ? id : next_auto_id_++;

    // Validation. Reject before touching any state so a bad order is a no-op.
    if (quantity <= 0 || price == kNoPrice || price == kInvalidPrice) {
        result.status = OrderStatus::Rejected;
        return result;
    }
    if (id != kInvalidOrderId && order_index_.count(id) != 0) {
        // Duplicate live id: reject rather than overwrite an existing order.
        result.status = OrderStatus::Rejected;
        return result;
    }

    Order order;
    order.id = result.id;
    order.participant = participant;
    order.side = side;
    order.type = OrderType::Limit;
    order.tif = tif;
    order.price = price;
    order.quantity = quantity;
    order.remaining = quantity;
    order.arrival = time;

    // Fill-or-kill: the order must be entirely fillable against currently
    // visible liquidity, otherwise it is cancelled in full without matching.
    if (order.tif == TimeInForce::FOK) {
        // Liquidity that a FOK buy can consume is on the *sell* side (and vice
        // versa): we ask the opposite side for depth at prices this order
        // would cross.
        const Quantity available = depth_at_least(opposite(side), price);
        if (available < quantity) {
            result.status = OrderStatus::Cancelled;
            return result;
        }
    }

    // An empty opposite side means nothing can cross; rest immediately.
    if (const auto best = best_price_unlocked(opposite(side)); best.has_value()) {
        if (crosses(side, price, *best)) {
            std::vector<Trade> trades;
            match_against(order, &trades);
            for (const Trade& t : trades) {
                result.filled += t.quantity;
                result.last_trade_price = t.price;
            }
        }
    }

        // Residual handling depends on the time-in-force. GTC rests the
        // remainder; IOC (and any non-GTC, non-FOK order) cancels it. A FOK
        // order reaching here was fully fillable, so it will have filled.
        if (order.remaining > 0)
        {
            if (order.tif == TimeInForce::GTC)
            {
                result.resting = order.remaining;
                result.status = result.filled > 0 ? OrderStatus::PartiallyFilled
                                                  : OrderStatus::New;
                rest(order);
            }
            else
            {
                result.status = result.filled > 0 ? OrderStatus::PartiallyFilled
                                                  : OrderStatus::Cancelled;
            }
        }
        else
        {
            result.status = OrderStatus::Filled;
        }
        return result;
    }

SubmitResult OrderBook::submit_market(Side side, Quantity quantity,
                                      ParticipantId participant, OrderId id,
                                      Timestamp time) {
    SubmitResult result;
    result.id = id != kInvalidOrderId ? id : next_auto_id_++;

    if (quantity <= 0) {
        result.status = OrderStatus::Rejected;
        return result;
    }
    if (id != kInvalidOrderId && order_index_.count(id) != 0) {
        result.status = OrderStatus::Rejected;
        return result;
    }

    Order order;
    order.id = result.id;
    order.participant = participant;
    order.side = side;
    order.type = OrderType::Market;
    order.tif = TimeInForce::IOC;
    order.price = kNoPrice;
    order.quantity = quantity;
    order.remaining = quantity;
    order.arrival = time;

    std::vector<Trade> trades;
    match_against(order, &trades);
    for (const Trade& t : trades) {
        result.filled += t.quantity;
        result.last_trade_price = t.price;
    }

    if (result.filled == quantity) {
        result.status = OrderStatus::Filled;
    } else if (result.filled == 0) {
        result.status = OrderStatus::Cancelled;  // nothing to hit: unfilled
    } else {
        result.status = OrderStatus::PartiallyFilled;  // remainder cancelled
    }
    result.resting = 0;  // market orders never rest
    return result;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

void OrderBook::match_against(Order& aggressor, std::vector<Trade>* trades) {
    const Side opp = opposite(aggressor.side);

    auto step = [&](auto& book) {
        while (aggressor.remaining > 0 && !book.empty()) {
            auto level_it = book.begin();
            const Price level_price = level_it->first;

            // Stop as soon as the aggressor no longer crosses this level. A
            // market order (kNoPrice sentinel) always crosses while liquidity
            // remains on the opposite side.
            if (aggressor.type == OrderType::Limit &&
                !crosses(aggressor.side, aggressor.price, level_price)) {
                break;
            }

            LevelState& level = level_it->second;
            while (aggressor.remaining > 0 && !level.orders.empty()) {
                Order& passive = level.orders.front();
                const Quantity traded = std::min(aggressor.remaining, passive.remaining);

                aggressor.remaining -= traded;
                passive.remaining -= traded;
                level.total_quantity -= traded;

                if (trades != nullptr) {
                    trades->push_back(Trade{aggressor.id,
                                            passive.id,
                                            aggressor.side,
                                            level_price,
                                            traded,
                                            aggressor.arrival});
                }

                if (passive.remaining == 0) {
                    passive.in_book = false;
                    order_index_.erase(passive.id);
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty()) {
                book.erase(level_it);
            }
        }
    };

    if (opp == Side::Sell) {
        step(asks_);
    } else {
        step(bids_);
    }
}

// ---------------------------------------------------------------------------
// Resting-order lifecycle
// ---------------------------------------------------------------------------

void OrderBook::rest(Order order) {
    order.in_book = true;
    order.remaining = order.remaining > 0 ? order.remaining : order.quantity;

    if (order.side == Side::Buy) {
        LevelState& level = bids_[order.price];
        level.orders.push_back(order);
        level.total_quantity += order.remaining;
        auto it = std::prev(level.orders.end());
        order_index_[order.id] = OrderLocator{Side::Buy, order.price, it};
    } else {
        LevelState& level = asks_[order.price];
        level.orders.push_back(order);
        level.total_quantity += order.remaining;
        auto it = std::prev(level.orders.end());
        order_index_[order.id] = OrderLocator{Side::Sell, order.price, it};
    }
}

void OrderBook::remove_resting(OrderId id) {
    auto index_it = order_index_.find(id);
    if (index_it == order_index_.end()) {
        return;
    }
    const OrderLocator loc = index_it->second;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        level_it->second.total_quantity -= loc.it->remaining;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(loc.price);
        level_it->second.total_quantity -= loc.it->remaining;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) {
            asks_.erase(level_it);
        }
    }
    order_index_.erase(index_it);
}

bool OrderBook::cancel(OrderId id) {
    auto index_it = order_index_.find(id);
    if (index_it == order_index_.end()) {
        return false;
    }
    remove_resting(id);
    return true;
}

bool OrderBook::reduce(OrderId id, Quantity new_quantity) {
    auto index_it = order_index_.find(id);
    if (index_it == order_index_.end()) {
        return false;
    }
    OrderLocator loc = index_it->second;
    if (new_quantity <= 0 || new_quantity >= loc.it->remaining) {
        return false;  // a full cancel is `cancel`; a no-op is not a reduce
    }
    const Quantity delta = loc.it->remaining - new_quantity;
    loc.it->remaining = new_quantity;

    if (loc.side == Side::Buy) {
        bids_.find(loc.price)->second.total_quantity -= delta;
    } else {
        asks_.find(loc.price)->second.total_quantity -= delta;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const Order* OrderBook::find(OrderId id) const {
    auto it = order_index_.find(id);
    return it == order_index_.end() ? nullptr : &(*it->second.it);
}

std::optional<Price> OrderBook::best_price_unlocked(Side side) const {
    if (side == Side::Buy) {
        return bids_.empty() ? std::nullopt
                             : std::optional<Price>(bids_.begin()->first);
    }
    return asks_.empty() ? std::nullopt
                         : std::optional<Price>(asks_.begin()->first);
}

std::optional<Price> OrderBook::best_price(Side side) const {
    return best_price_unlocked(side);
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_quantity;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_quantity;
}

TopOfBook OrderBook::top_of_book() const {
    TopOfBook tob;
    if (!bids_.empty()) {
        const auto& [price, level] = *bids_.begin();
        tob.bid = Level{price, level.total_quantity, level.orders.size()};
    }
    if (!asks_.empty()) {
        const auto& [price, level] = *asks_.begin();
        tob.ask = Level{price, level.total_quantity, level.orders.size()};
    }
    return tob;
}

std::optional<double> OrderBook::mid_price() const {
    const auto bid = best_price_unlocked(Side::Buy);
    const auto ask = best_price_unlocked(Side::Sell);
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    return 0.5 * (static_cast<double>(*bid) + static_cast<double>(*ask));
}

std::optional<Price> OrderBook::spread() const {
    const auto bid = best_price_unlocked(Side::Buy);
    const auto ask = best_price_unlocked(Side::Sell);
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::size_t OrderBook::level_count(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

Quantity OrderBook::total_quantity(Side side) const {
    Quantity total = 0;
    if (side == Side::Buy) {
        for (const auto& [price, level] : bids_) {
            (void)price;
            total += level.total_quantity;
        }
    } else {
        for (const auto& [price, level] : asks_) {
            (void)price;
            total += level.total_quantity;
        }
    }
    return total;
}

Quantity OrderBook::depth_at_least(Side side, Price price) const {
    Quantity total = 0;
    if (side == Side::Buy) {
        // Bids at or above `price` are aggressive enough to cross a sell at
        // `price`; std::greater ordering means we scan from the top down.
        for (const auto& [level_price, level] : bids_) {
            if (level_price < price) {
                break;
            }
            total += level.total_quantity;
        }
    } else {
        for (const auto& [level_price, level] : asks_) {
            if (level_price > price) {
                break;
            }
            total += level.total_quantity;
        }
    }
    return total;
}

std::vector<Level> OrderBook::levels(Side side, std::size_t max_levels) const {
    std::vector<Level> out;
    out.reserve(std::min<std::size_t>(max_levels, level_count(side)));
    if (side == Side::Buy) {
        for (const auto& [price, level] : bids_) {
            if (out.size() == max_levels) {
                break;
            }
            out.push_back(Level{price, level.total_quantity, level.orders.size()});
        }
    } else {
        for (const auto& [price, level] : asks_) {
            if (out.size() == max_levels) {
                break;
            }
            out.push_back(Level{price, level.total_quantity, level.orders.size()});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Invariant checks
// ---------------------------------------------------------------------------

void OrderBook::check_invariants() const {
    std::size_t counted = 0;

    auto check_side = [&](auto& book, Side side, const char* name) {
        for (const auto& [price, level] : book) {
            if (level.orders.empty()) {
                fail(std::string(name) + ": empty level left in book");
            }
            Quantity summed = 0;
            for (const auto& order : level.orders) {
                if (!order.in_book) fail(std::string(name) + ": non-resting order in FIFO");
                if (order.remaining <= 0) fail(std::string(name) + ": non-positive remaining");
                if (order.side != side) fail(std::string(name) + ": wrong-side order in level");
                if (order.price != price) fail(std::string(name) + ": order at wrong price");
                summed += order.remaining;

                auto idx = order_index_.find(order.id);
                if (idx == order_index_.end()) fail(std::string(name) + ": order missing from index");
                if (idx->second.side != side || idx->second.price != price) {
                    fail(std::string(name) + ": index disagrees with level");
                }
                if (&(*idx->second.it) != &order) fail(std::string(name) + ": index points elsewhere");
                ++counted;
            }
            if (summed != level.total_quantity) {
                fail(std::string(name) + ": level aggregate != sum of orders");
            }
        }
    };

    check_side(bids_, Side::Buy, "bids");
    check_side(asks_, Side::Sell, "asks");

    if (counted != order_index_.size()) {
        fail("index has stale entries not present in any level");
    }

    const auto bid = best_price_unlocked(Side::Buy);
    const auto ask = best_price_unlocked(Side::Sell);
    if (bid.has_value() && ask.has_value() && *bid >= *ask) {
        fail("crossed book: best_bid >= best_ask");
    }
}

}  // namespace mercury
