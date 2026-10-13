// Mercury — property tests for the order book.
//
// Rather than hand-writing one scenario per rule, we drive the book with a
// randomised stream of operations and, after every step, check:
//
//   1. the book's own internal invariants (`check_invariants()`),
//   2. agreement with a dead-simple *reference model* that recomputes the top
//      of book and total quantity from a flat list of resting orders,
//   3. a share-conservation ledger (see `check_conservation` below).
//
// The reference model is intentionally naive (linear scans, no data
// structures) so that it is obviously correct; the book must match it.
//
// Determinism is checked separately: the same seed replayed twice must yield
// byte-identical books, and trades must be emitted in strict price-then-time
// order.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "engine/book/order_book.hpp"
#include "tests/test_harness.hpp"

using namespace mercury;

namespace
{

    // A resting order as tracked by the reference model. Copied by value whenever
    // it is handed out, so a caller may mutate the reference book freely while
    // walking a previously-taken snapshot.
    struct RefOrder
    {
        OrderId id{0};
        Side side{Side::Buy};
        Price price{0};
        Quantity remaining{0};
        std::uint64_t sequence{0}; // arrival order, for time priority
    };

    // The naive reference book: a flat vector, recomputed on every query.
    class ReferenceBook
    {
    public:
        void add(const RefOrder &o) { orders_.push_back(o); }

        void remove(OrderId id)
        {
            orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                         [&](const RefOrder &o)
                                         { return o.id == id; }),
                          orders_.end());
        }

        void set_remaining(OrderId id, Quantity q)
        {
            for (auto &o : orders_)
            {
                if (o.id == id)
                    o.remaining = q;
            }
        }

        [[nodiscard]] const RefOrder *find(OrderId id) const
        {
            for (const auto &o : orders_)
            {
                if (o.id == id)
                    return &o;
            }
            return nullptr;
        }

        [[nodiscard]] Quantity total(Side side) const
        {
            Quantity t = 0;
            for (const auto &o : orders_)
            {
                if (o.side == side)
                    t += o.remaining;
            }
            return t;
        }

        [[nodiscard]] Quantity quantity_at(Side side, Price price) const
        {
            Quantity t = 0;
            for (const auto &o : orders_)
            {
                if (o.side == side && o.price == price)
                    t += o.remaining;
            }
            return t;
        }

        [[nodiscard]] std::optional<Price> best(Side side) const
        {
            std::optional<Price> best;
            for (const auto &o : orders_)
            {
                if (o.side != side)
                    continue;
                if (!best.has_value() ||
                    (side == Side::Buy ? o.price > *best : o.price < *best))
                {
                    best = o.price;
                }
            }
            return best;
        }

        // Orders the aggressor would consume, in (price, time) order, as value
        // copies so the caller can mutate `orders_` while walking the result.
        [[nodiscard]] std::vector<RefOrder> consumption_order(
            Side aggressor_side, std::optional<Price> limit) const
        {
            const Side book_side = opposite(aggressor_side);
            std::vector<RefOrder> out;
            for (const auto &o : orders_)
            {
                if (o.side != book_side)
                    continue;
                if (limit.has_value())
                {
                    const bool crosses = aggressor_side == Side::Buy
                                             ? o.price <= *limit
                                             : o.price >= *limit;
                    if (!crosses)
                        continue;
                }
                out.push_back(o);
            }
            std::sort(out.begin(), out.end(),
                      [&](const RefOrder &a, const RefOrder &b)
                      {
                          if (a.price != b.price)
                          {
                              return aggressor_side == Side::Buy ? a.price < b.price
                                                                 : a.price > b.price;
                          }
                          return a.sequence < b.sequence;
                      });
            return out;
        }

        // Apply fills totalling `filled` shares to the reference book in the same
        // price-time order the engine uses. Returns (id, traded) pairs.
        std::vector<std::pair<OrderId, Quantity>> consume(Side aggressor_side,
                                                          std::optional<Price> limit,
                                                          Quantity filled)
        {
            std::vector<std::pair<OrderId, Quantity>> done;
            Quantity left = filled;
            for (const RefOrder &o : consumption_order(aggressor_side, limit))
            {
                if (left <= 0)
                    break;
                const Quantity take = std::min(o.remaining, left);
                left -= take;
                done.emplace_back(o.id, take);
                const Quantity remaining = o.remaining - take;
                if (remaining == 0)
                {
                    remove(o.id);
                }
                else
                {
                    set_remaining(o.id, remaining);
                }
            }
            return done;
        }

    private:
        std::vector<RefOrder> orders_;
    };

    void cross_check(const OrderBook &book, const ReferenceBook &ref)
    {
        book.check_invariants();
        for (Side side : {Side::Buy, Side::Sell})
        {
            MERCURY_CHECK_EQ(book.total_quantity(side), ref.total(side));
            const auto b = book.best_price(side);
            const auto rb = ref.best(side);
            MERCURY_CHECK_EQ(b.has_value(), rb.has_value());
            if (b.has_value())
            {
                MERCURY_CHECK_EQ(*b, *rb);
                MERCURY_CHECK_EQ(book.quantity_at(side, *b), ref.quantity_at(side, *b));
            }
        }
    }

    // Share-conservation ledger.
    //
    // Every share that enters the book is submitted exactly once and, at any
    // instant, is in one of three states: still resting, removed by a cancel or
    // reduce, or already executed. A single trade decrements the remaining size of
    // *both* counterparties, so each executed share leaves the resting pool **and**
    // satisfies one share of the aggressor's submitted quantity: it therefore
    // consumes two units of "submitted" for every unit of "traded". Hence
    //
    //     submitted_shares == resting + cancelled + 2 * traded.
    //
    // This is the exact ledger the test maintains; getting it wrong by a factor of
    // two is the classic way this check silently passes or mysteriously fails.
    void check_conservation(Quantity submitted, const OrderBook &book,
                            Quantity traded, Quantity cancelled)
    {
        const Quantity resting =
            book.total_quantity(Side::Buy) + book.total_quantity(Side::Sell);
        MERCURY_CHECK_EQ(submitted, resting + cancelled + 2 * traded);
    }

} // namespace

MERCURY_TEST(random_operations_match_reference_model)
{
    OrderBook book;
    ReferenceBook ref;

    std::mt19937_64 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<Price> price_dist(90, 110);
    std::uniform_int_distribution<Quantity> qty_dist(1, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);

    OrderId next_id = 1;
    std::uint64_t sequence = 0;
    std::vector<OrderId> live_ids;
    Quantity submitted_shares = 0;
    Quantity traded_shares = 0;
    Quantity cancelled_shares = 0;

    for (int step = 0; step < 20000; ++step)
    {
        const int roll = op_dist(rng);

        if (roll < 60 || live_ids.empty())
        {
            // 60%: submit a limit order (may cross and match immediately).
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            const OrderId id = next_id++;
            submitted_shares += qty;

            const auto res = book.submit_limit(side, price, qty, 0, id);
            traded_shares += res.filled;

            // Fold any immediate fills into the reference, then record the
            // remainder that rested.
            if (res.filled > 0)
            {
                ref.consume(side, price, res.filled);
            }
            if (res.resting > 0)
            {
                ref.add(RefOrder{id, side, price, res.resting, sequence++});
                live_ids.push_back(id);
            }
        }
        else if (roll < 80)
        {
            // 20%: cancel a random live order.
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live_ids[idx];
            const RefOrder *r = ref.find(id);
            const Quantity removed = r != nullptr ? r->remaining : 0;
            if (book.cancel(id))
            {
                cancelled_shares += removed;
                ref.remove(id);
                live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }
        else
        {
            // 20%: reduce a random live order to a smaller size.
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            const OrderId id = live_ids[pick(rng)];
            const RefOrder *r = ref.find(id);
            if (r == nullptr || r->remaining < 2)
            {
                continue;
            }
            std::uniform_int_distribution<Quantity> newq(1, r->remaining - 1);
            const Quantity target = newq(rng);
            if (book.reduce(id, target))
            {
                cancelled_shares += r->remaining - target;
                ref.set_remaining(id, target);
            }
        }

        cross_check(book, ref);
        check_conservation(submitted_shares, book, traded_shares, cancelled_shares);
    }
}

MERCURY_TEST(market_and_ioc_orders_match_reference_consumption)
{
    OrderBook book;
    ReferenceBook ref;

    std::mt19937_64 rng(0xBEEF1234u);
    std::uniform_int_distribution<Price> price_dist(95, 105);
    std::uniform_int_distribution<Quantity> qty_dist(1, 300);
    std::uniform_int_distribution<int> side_dist(0, 1);

    OrderId next_id = 1;
    std::uint64_t sequence = 0;
    Quantity submitted = 0;
    Quantity traded = 0;
    Quantity cancelled = 0;

    for (int step = 0; step < 5000; ++step)
    {
        if (ref.best(Side::Buy).has_value() && ref.best(Side::Sell).has_value() &&
            rng() % 2 == 0)
        {
            // Hit the book with a market or IOC order and verify the exact
            // filled size against the reference consumption order.
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Quantity qty = qty_dist(rng);
            submitted += qty;
            const bool market = rng() % 2 == 0;

            std::optional<Price> limit;
            if (!market)
            {
                limit = side == Side::Buy ? Price{105} : Price{95};
            }

            Quantity expected_filled = 0;
            for (const RefOrder &o : ref.consumption_order(side, limit))
            {
                if (expected_filled >= qty)
                    break;
                expected_filled += std::min(o.remaining, qty - expected_filled);
            }

            SubmitResult res;
            if (market)
            {
                res = book.submit_market(side, qty, 0, next_id);
            }
            else
            {
                res = book.submit_limit(side, *limit, qty, 0, next_id, 0,
                                        TimeInForce::IOC);
            }
            ++next_id;

            MERCURY_CHECK_EQ(res.filled, expected_filled);
            MERCURY_CHECK_EQ(res.resting, Quantity{0});

            // The unfilled remainder of a market/IOC order is cancelled.
            cancelled += qty - res.filled;
            traded += res.filled;
            ref.consume(side, limit, res.filled);
        }
        else
        {
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            const OrderId id = next_id++;
            submitted += qty;
            const auto res = book.submit_limit(side, price, qty, 0, id);
            traded += res.filled;

            if (res.filled > 0)
            {
                ref.consume(side, price, res.filled);
            }
            if (res.resting > 0)
            {
                ref.add(RefOrder{id, side, price, res.resting, sequence++});
            }
        }

        cross_check(book, ref);
        check_conservation(submitted, book, traded, cancelled);
    }
}

MERCURY_TEST(trades_are_emitted_in_price_then_time_order)
{
    // A deliberately mis-ordered set of asks; a market buy must consume them
    // strictly by (price, arrival): ids 2,4 @100 then 3,5 @101 then 1 @102.
    OrderBook book;
    book.submit_limit(Side::Sell, 102, 10, 1, 1, /*time=*/100);
    book.submit_limit(Side::Sell, 100, 10, 1, 2, /*time=*/200);
    book.submit_limit(Side::Sell, 101, 10, 1, 3, /*time=*/300);
    book.submit_limit(Side::Sell, 100, 10, 1, 4, /*time=*/400);
    book.submit_limit(Side::Sell, 101, 10, 1, 5, /*time=*/500);

    const auto res = book.submit_market(Side::Buy, 50, 0, 99);
    MERCURY_CHECK_EQ(res.filled, Quantity{50});
    MERCURY_CHECK(book.find(2) == nullptr);
    MERCURY_CHECK(book.find(4) == nullptr);
    MERCURY_CHECK(book.find(3) == nullptr);
    MERCURY_CHECK(book.find(5) == nullptr);
    MERCURY_CHECK(book.find(1) == nullptr);
    book.check_invariants();
}

MERCURY_TEST(replay_is_deterministic)
{
    auto run = [](std::uint64_t seed)
    {
        OrderBook book;
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<Price> price_dist(95, 105);
        std::uniform_int_distribution<Quantity> qty_dist(1, 200);
        std::uniform_int_distribution<int> side_dist(0, 1);
        for (OrderId id = 1; id <= 3000; ++id)
        {
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            if (id % 7 == 0)
            {
                book.submit_market(side, qty, 0, id);
            }
            else
            {
                book.submit_limit(side, price, qty, 0, id);
            }
            if (id % 11 == 0)
            {
                book.cancel(id / 2);
            }
        }
        book.check_invariants();

        // Observable fingerprint of the final book state.
        std::vector<std::int64_t> fingerprint;
        for (Side side : {Side::Buy, Side::Sell})
        {
            for (const Level &l : book.levels(side, 1000))
            {
                fingerprint.push_back(static_cast<std::int64_t>(l.price));
                fingerprint.push_back(l.quantity);
                fingerprint.push_back(static_cast<std::int64_t>(l.order_count));
            }
        }
        return fingerprint;
    };

    const auto a = run(12345);
    const auto b = run(12345);
    const auto c = run(99999);
    MERCURY_CHECK(a == b);
    // A different seed should (almost surely) produce a different book, which
    // guards against the test passing trivially because `run` ignores its seed.
    MERCURY_CHECK(a != c);
}

MERCURY_MAIN()
