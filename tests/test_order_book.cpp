// Mercury — invariant tests for the price-time-priority order book.
//
// These are the tests the implementation plan's Phase 2 gate refers to:
// they exercise the documented semantics (price priority, time priority,
// partial fills, cancels, TIF handling) and call `check_invariants()` after
// every operation so that any inconsistency is caught at the point it appears.

#include "engine/book/order_book.hpp"
#include "tests/test_harness.hpp"

using namespace mercury;

namespace {

// Fill a book with a simple, known shape: bids 99/98, asks 101/102.
void seed(OrderBook& book) {
    book.submit_limit(Side::Buy, 99, 100, /*participant=*/1, /*id=*/1);
    book.submit_limit(Side::Buy, 98, 200, 1, 2);
    book.submit_limit(Side::Sell, 101, 150, 2, 3);
    book.submit_limit(Side::Sell, 102, 250, 2, 4);
    book.check_invariants();
}

}  // namespace

MERCURY_TEST(fresh_book_is_empty) {
    OrderBook book;
    MERCURY_CHECK(book.empty());
    MERCURY_CHECK_EQ(book.resting_order_count(), std::size_t{0});
    MERCURY_CHECK(!book.best_price(Side::Buy).has_value());
    MERCURY_CHECK(!book.best_price(Side::Sell).has_value());
    MERCURY_CHECK(!book.mid_price().has_value());
    book.check_invariants();
}

MERCURY_TEST(limit_orders_rest_and_set_top_of_book) {
    OrderBook book;
    seed(book);

    MERCURY_CHECK_EQ(*book.best_price(Side::Buy), Price{99});
    MERCURY_CHECK_EQ(*book.best_price(Side::Sell), Price{101});
    MERCURY_CHECK_EQ(*book.spread(), Price{2});
    MERCURY_CHECK_EQ(*book.mid_price(), 100.0);
    MERCURY_CHECK_EQ(book.quantity_at(Side::Buy, 99), Quantity{100});
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 101), Quantity{150});
    MERCURY_CHECK_EQ(book.total_quantity(Side::Buy), Quantity{300});
    MERCURY_CHECK_EQ(book.total_quantity(Side::Sell), Quantity{400});

    const auto tob = book.top_of_book();
    MERCURY_CHECK(tob.bid.has_value() && tob.ask.has_value());
    MERCURY_CHECK_EQ(tob.bid->price, Price{99});
    MERCURY_CHECK_EQ(tob.bid->order_count, std::size_t{1});
    MERCURY_CHECK_EQ(tob.ask->price, Price{101});
}

MERCURY_TEST(rejected_orders_are_noops) {
    OrderBook book;
    seed(book);
    const auto before = book.resting_order_count();

    MERCURY_CHECK_EQ(book.submit_limit(Side::Buy, 99, 0, 1, 10).status,
                     OrderStatus::Rejected);
    MERCURY_CHECK_EQ(book.submit_limit(Side::Buy, 99, -5, 1, 11).status,
                     OrderStatus::Rejected);
    MERCURY_CHECK_EQ(book.submit_limit(Side::Buy, kNoPrice, 10, 1, 12).status,
                     OrderStatus::Rejected);
    MERCURY_CHECK_EQ(book.submit_market(Side::Buy, 0, 1, 13).status,
                     OrderStatus::Rejected);

    MERCURY_CHECK_EQ(book.resting_order_count(), before);
    book.check_invariants();
}

MERCURY_TEST(duplicate_live_id_is_rejected) {
    OrderBook book;
    seed(book);

    // Id 1 is already resting at 99.
    const auto res = book.submit_limit(Side::Buy, 97, 50, 1, /*id=*/1);
    MERCURY_CHECK_EQ(res.status, OrderStatus::Rejected);
    MERCURY_CHECK_EQ(book.quantity_at(Side::Buy, 99), Quantity{100});
    MERCURY_CHECK(!book.quantity_at(Side::Buy, 97));
    book.check_invariants();
}

MERCURY_TEST(price_priority_aggressor_hits_best_then_next) {
    OrderBook book;
    seed(book);

    // Buy 200 @ 102: should sweep 101 (150) then 50 of 102.
    const auto res = book.submit_limit(Side::Buy, 102, 200, /*participant=*/9,
                                       /*id=*/100);
    MERCURY_CHECK_EQ(res.filled, Quantity{200});
    MERCURY_CHECK_EQ(res.resting, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::Filled);
    MERCURY_CHECK_EQ(res.last_trade_price, Price{102});

    MERCURY_CHECK(!book.quantity_at(Side::Sell, 101));       // fully consumed
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 102), Quantity{200});  // 250-50
    MERCURY_CHECK_EQ(*book.best_price(Side::Sell), Price{102});
    book.check_invariants();
}

MERCURY_TEST(time_priority_within_a_level) {
    OrderBook book;
    book.submit_limit(Side::Sell, 101, 100, /*participant=*/1, /*id=*/1);
    book.submit_limit(Side::Sell, 101, 100, /*participant=*/2, /*id=*/2);
    book.check_invariants();

    // Buy 100: the first resting sell (id 1) must be the one filled.
    const auto res = book.submit_limit(Side::Buy, 101, 100, 9, 10);
    MERCURY_CHECK_EQ(res.filled, Quantity{100});
    MERCURY_CHECK(book.find(/*id=*/1) == nullptr);  // consumed
    MERCURY_CHECK(book.find(/*id=*/2) != nullptr);  // untouched
    MERCURY_CHECK_EQ(book.find(2)->remaining, Quantity{100});
    book.check_invariants();
}

MERCURY_TEST(partial_fill_of_resting_order) {
    OrderBook book;
    book.submit_limit(Side::Sell, 100, 300, 1, 1);
    const auto res = book.submit_limit(Side::Buy, 100, 120, 2, 2);
    MERCURY_CHECK_EQ(res.filled, Quantity{120});
    MERCURY_CHECK_EQ(res.resting, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::Filled);

    const Order* resting = book.find(1);
    MERCURY_CHECK(resting != nullptr);
    MERCURY_CHECK_EQ(resting->remaining, Quantity{180});
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 100), Quantity{180});
    book.check_invariants();
}

MERCURY_TEST(aggressor_rests_residual) {
    OrderBook book;
    book.submit_limit(Side::Sell, 100, 40, 1, 1);

    // Buy 100 @ 100 crosses 40 and rests 60.
    const auto res = book.submit_limit(Side::Buy, 100, 100, 2, 2);
    MERCURY_CHECK_EQ(res.filled, Quantity{40});
    MERCURY_CHECK_EQ(res.resting, Quantity{60});
    MERCURY_CHECK_EQ(res.status, OrderStatus::PartiallyFilled);
    MERCURY_CHECK_EQ(*book.best_price(Side::Buy), Price{100});
    MERCURY_CHECK_EQ(book.quantity_at(Side::Buy, 100), Quantity{60});
    book.check_invariants();
}

MERCURY_TEST(ioc_limit_cancels_remainder) {
    OrderBook book;
    book.submit_limit(Side::Sell, 100, 40, 1, 1);

    const auto res = book.submit_limit(Side::Buy, 100, 100, 2, 2, /*time=*/0,
                                       TimeInForce::IOC);
    MERCURY_CHECK_EQ(res.filled, Quantity{40});
    MERCURY_CHECK_EQ(res.resting, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::PartiallyFilled);
    MERCURY_CHECK(!book.best_price(Side::Buy).has_value());
    book.check_invariants();
}

MERCURY_TEST(ioc_limit_with_no_cross_is_cancelled) {
    OrderBook book;
    book.submit_limit(Side::Sell, 101, 40, 1, 1);
    const auto res = book.submit_limit(Side::Buy, 100, 100, 2, 2, 0,
                                       TimeInForce::IOC);
    MERCURY_CHECK_EQ(res.filled, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::Cancelled);
    MERCURY_CHECK(!book.best_price(Side::Buy).has_value());
    book.check_invariants();
}

MERCURY_TEST(fok_all_or_nothing) {
    OrderBook book;
    book.submit_limit(Side::Sell, 100, 40, 1, 1);
    book.submit_limit(Side::Sell, 101, 10, 1, 2);

    // 60 available at <= 101, request 50: fillable -> fully filled.
    const auto ok = book.submit_limit(Side::Buy, 101, 50, 2, 3, 0, TimeInForce::FOK);
    MERCURY_CHECK_EQ(ok.filled, Quantity{50});
    MERCURY_CHECK_EQ(ok.status, OrderStatus::Filled);
    MERCURY_CHECK(!book.quantity_at(Side::Sell, 100));
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 101), Quantity{0});

    // Only 0 left now; a FOK buy of 1 must be cancelled whole.
    book.submit_limit(Side::Sell, 105, 5, 1, 4);
    const auto kill = book.submit_limit(Side::Buy, 101, 1, 2, 5, 0, TimeInForce::FOK);
    MERCURY_CHECK_EQ(kill.filled, Quantity{0});
    MERCURY_CHECK_EQ(kill.status, OrderStatus::Cancelled);
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 105), Quantity{5});
    book.check_invariants();
}

MERCURY_TEST(market_order_sweeps_and_never_rests) {
    OrderBook book;
    seed(book);

    // Buy market 200: fills 150 @101 then 50 @102, no remainder.
    const auto res = book.submit_market(Side::Buy, 200, 9, 50);
    MERCURY_CHECK_EQ(res.filled, Quantity{200});
    MERCURY_CHECK_EQ(res.resting, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::Filled);
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 102), Quantity{200});
    MERCURY_CHECK(book.find(50) == nullptr);  // market orders are not indexed
    book.check_invariants();
}

MERCURY_TEST(market_order_insufficient_liquidity_is_partial) {
    OrderBook book;
    book.submit_limit(Side::Sell, 100, 30, 1, 1);

    const auto res = book.submit_market(Side::Buy, 100, 2, 2);
    MERCURY_CHECK_EQ(res.filled, Quantity{30});
    MERCURY_CHECK_EQ(res.resting, Quantity{0});
    MERCURY_CHECK_EQ(res.status, OrderStatus::PartiallyFilled);
    MERCURY_CHECK(book.empty());
    book.check_invariants();
}

MERCURY_TEST(cancel_removes_liquidity_and_level) {
    OrderBook book;
    book.submit_limit(Side::Buy, 99, 100, 1, 1);
    book.submit_limit(Side::Buy, 99, 50, 1, 2);
    book.submit_limit(Side::Buy, 98, 10, 1, 3);

    MERCURY_CHECK(book.cancel(1));
    MERCURY_CHECK(book.find(1) == nullptr);
    MERCURY_CHECK_EQ(book.quantity_at(Side::Buy, 99), Quantity{50});

    MERCURY_CHECK(book.cancel(2));
    // Level 99 is now empty and must have been pruned.
    MERCURY_CHECK_EQ(*book.best_price(Side::Buy), Price{98});
    MERCURY_CHECK_EQ(book.level_count(Side::Buy), std::size_t{1});

    MERCURY_CHECK(!book.cancel(999));  // unknown id
    MERCURY_CHECK(!book.cancel(2));    // already cancelled
    book.check_invariants();
}

MERCURY_TEST(reduce_keeps_time_priority) {
    OrderBook book;
    book.submit_limit(Side::Sell, 101, 100, 1, 1);  // oldest
    book.submit_limit(Side::Sell, 101, 100, 2, 2);  // newer

    MERCURY_CHECK(book.reduce(1, 30));
    MERCURY_CHECK_EQ(book.find(1)->remaining, Quantity{30});
    MERCURY_CHECK_EQ(book.quantity_at(Side::Sell, 101), Quantity{130});

    // Order 1 is still first in time priority, so it fills before order 2.
    const auto res = book.submit_limit(Side::Buy, 101, 40, 9, 10);
    MERCURY_CHECK_EQ(res.filled, Quantity{40});
    MERCURY_CHECK(book.find(1) == nullptr);           // 30 consumed, gone
    MERCURY_CHECK_EQ(book.find(2)->remaining, Quantity{90});  // 10 consumed
    book.check_invariants();
}

MERCURY_TEST(reduce_rejects_invalid_requests) {
    OrderBook book;
    book.submit_limit(Side::Sell, 101, 100, 1, 1);

    MERCURY_CHECK(!book.reduce(1, 100));  // no change
    MERCURY_CHECK(!book.reduce(1, 150));  // increase
    MERCURY_CHECK(!book.reduce(1, 0));    // zero -> use cancel
    MERCURY_CHECK(!book.reduce(999, 10)); // unknown
    MERCURY_CHECK_EQ(book.find(1)->remaining, Quantity{100});
    book.check_invariants();
}

MERCURY_TEST(levels_and_depth_views) {
    OrderBook book;
    book.submit_limit(Side::Sell, 101, 10, 1, 1);
    book.submit_limit(Side::Sell, 102, 20, 1, 2);
    book.submit_limit(Side::Sell, 103, 30, 1, 3);

    const auto lv = book.levels(Side::Sell, 2);
    MERCURY_CHECK_EQ(lv.size(), std::size_t{2});
    MERCURY_CHECK_EQ(lv[0].price, Price{101});
    MERCURY_CHECK_EQ(lv[1].price, Price{102});

    MERCURY_CHECK_EQ(book.depth_at_least(Side::Sell, 102), Quantity{30});
    MERCURY_CHECK_EQ(book.depth_at_least(Side::Sell, 103), Quantity{60});
    MERCURY_CHECK_EQ(book.depth_at_least(Side::Sell, 100), Quantity{0});

    // A buy-side depth query for a bid level that does not exist is zero.
    MERCURY_CHECK_EQ(book.depth_at_least(Side::Buy, 0), Quantity{0});
    book.check_invariants();
}

MERCURY_TEST(auto_ids_do_not_collide_with_explicit_live_ids) {
    OrderBook b;
    b.submit_limit(Side::Buy, 100, 50, 1, 1);
    const auto result = b.submit_limit(Side::Buy, 100, 60);
    MERCURY_CHECK_EQ(result.id, 2U);
    MERCURY_CHECK_EQ(b.quantity_at(Side::Buy, 100), 110);
    MERCURY_CHECK(b.cancel(1));
    b.check_invariants();
    MERCURY_CHECK_EQ(b.quantity_at(Side::Buy, 100), 60);
}
MERCURY_TEST(extreme_prices_rejected_before_arithmetic) {
    OrderBook b;
    const auto result = b.submit_limit(Side::Buy, std::numeric_limits<Price>::min()+1, 50);
    MERCURY_CHECK_EQ(result.status, OrderStatus::Rejected);
    MERCURY_CHECK(b.empty());
}
MERCURY_MAIN()
