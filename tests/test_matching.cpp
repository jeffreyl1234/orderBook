#include "ob/order_book.hpp"
#include "test_framework.hpp"

using namespace ob;

// ---------------------------------------------------------------------------
// Resting / no-cross behaviour
// ---------------------------------------------------------------------------

TEST_CASE("limit that does not cross just rests") {
    OrderBook b;
    auto t1 = b.add_limit(1, Side::Buy, 100, 10);
    auto t2 = b.add_limit(2, Side::Sell, 101, 10);  // spread, no cross
    CHECK(t1.empty());
    CHECK(t2.empty());
    CHECK_EQ(b.best_bid().value(), 100);
    CHECK_EQ(b.best_ask().value(), 101);
    CHECK_EQ(b.order_count(), 2u);
}

TEST_CASE("empty book has no best bid or ask") {
    OrderBook b;
    CHECK(!b.best_bid().has_value());
    CHECK(!b.best_ask().has_value());
    CHECK(b.empty());
}

// ---------------------------------------------------------------------------
// Crossing / fills
// ---------------------------------------------------------------------------

TEST_CASE("exact full fill removes both orders") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 1u);
    CHECK_EQ(trades[0].taker_id, 2u);
    CHECK_EQ(trades[0].price, 100);       // maker's price
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK(b.empty());                     // both fully consumed
}

TEST_CASE("aggressor larger than resting: partial fill, remainder rests") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 100, 25);  // wants 25, only 10 avail
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK(!b.best_ask().has_value());     // ask consumed
    CHECK_EQ(b.best_bid().value(), 100);  // 15 of the buy now rests
    CHECK_EQ(b.quantity_at(Side::Buy, 100), 15u);
}

TEST_CASE("resting larger than aggressor: maker partially filled, stays") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 30);
    auto trades = b.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK_EQ(b.quantity_at(Side::Sell, 100), 20u);  // 20 left resting
    CHECK(!b.best_bid().has_value());               // buy fully filled
}

TEST_CASE("buy limit above ask trades at the ask price, not the limit") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 105, 10);  // willing to pay 105
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].price, 100);  // price improvement to the maker's 100
}

// ---------------------------------------------------------------------------
// Price-time priority
// ---------------------------------------------------------------------------

TEST_CASE("time priority: earlier order at same price fills first") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);   // arrives first
    b.add_limit(2, Side::Sell, 100, 10);   // same price, later
    auto trades = b.add_limit(3, Side::Buy, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 1u);      // FIFO: order 1 before order 2
    CHECK_EQ(b.quantity_at(Side::Sell, 100), 10u);
    CHECK(b.contains(2));
    CHECK(!b.contains(1));
}

TEST_CASE("price priority: better-priced level fills first") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 102, 10);
    b.add_limit(2, Side::Sell, 100, 10);   // better (lower) ask
    b.add_limit(3, Side::Sell, 101, 10);
    auto trades = b.add_limit(4, Side::Buy, 105, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 2u);      // lowest ask 100 first
    CHECK_EQ(trades[0].price, 100);
}

TEST_CASE("aggressor sweeps multiple price levels in order") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 101, 10);
    b.add_limit(3, Side::Sell, 102, 10);
    auto trades = b.add_limit(4, Side::Buy, 102, 25);  // eats 100,101, part of 102
    REQUIRE(trades.size() == 3);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[1].price, 101);
    CHECK_EQ(trades[2].price, 102);
    CHECK_EQ(trades[2].quantity, 5u);
    CHECK_EQ(b.quantity_at(Side::Sell, 102), 5u);
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST_CASE("market order sweeps book regardless of price") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 200, 10);   // far away, still eaten
    auto trades = b.add_market(3, Side::Buy, 15);
    REQUIRE(trades.size() == 2);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[1].price, 200);
    CHECK_EQ(trades[1].quantity, 5u);
}

TEST_CASE("market order with insufficient liquidity discards remainder") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_market(2, Side::Buy, 50);  // only 10 available
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK(b.empty());                              // remainder does NOT rest
}

TEST_CASE("market order against empty book does nothing") {
    OrderBook b;
    auto trades = b.add_market(1, Side::Buy, 10);
    CHECK(trades.empty());
    CHECK(b.empty());
}

// ---------------------------------------------------------------------------
// Cancels & empty-level cleanup
// ---------------------------------------------------------------------------

TEST_CASE("cancel removes order and empties the price level") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);
    CHECK_EQ(b.bid_levels(), 1u);
    CHECK(b.cancel(1));
    CHECK(!b.contains(1));
    CHECK_EQ(b.bid_levels(), 0u);          // level torn down when last order gone
    CHECK(!b.best_bid().has_value());
}

TEST_CASE("cancel one of several at a level keeps the level") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);
    b.add_limit(2, Side::Buy, 100, 10);
    CHECK(b.cancel(1));
    CHECK_EQ(b.bid_levels(), 1u);
    CHECK_EQ(b.quantity_at(Side::Buy, 100), 10u);
}

TEST_CASE("cancel unknown id returns false") {
    OrderBook b;
    CHECK(!b.cancel(999));
}

// ---------------------------------------------------------------------------
// Modify semantics
// ---------------------------------------------------------------------------

TEST_CASE("modify downsize at same price keeps time priority") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 20);   // first in queue
    b.add_limit(2, Side::Buy, 100, 20);
    std::vector<Trade> out;
    b.modify(1, 100, 5, out);             // shrink 20 -> 5
    CHECK(out.empty());
    CHECK_EQ(b.quantity_at(Side::Buy, 100), 25u);  // 5 + 20

    // Order 1 kept priority: an incoming sell hits it first.
    auto trades = b.add_limit(3, Side::Sell, 100, 5);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 1u);
}

TEST_CASE("modify price change loses priority and can cross") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);
    b.add_limit(2, Side::Sell, 105, 10);
    std::vector<Trade> out;
    b.modify(1, 105, 10, out);            // reprice the buy up to 105 -> crosses
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0].maker_id, 2u);
    CHECK_EQ(out[0].price, 105);
    CHECK(b.empty());
}

TEST_CASE("modify size increase loses priority (cancel + re-add)") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);   // first
    b.add_limit(2, Side::Buy, 100, 10);   // second
    std::vector<Trade> out;
    b.modify(1, 100, 30, out);            // grow -> goes to back of the queue
    auto trades = b.add_limit(3, Side::Sell, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 2u);     // order 2 now ahead of re-added 1
}

// ---------------------------------------------------------------------------
// Passive primitives (the path ITCH replay drives)
// ---------------------------------------------------------------------------

TEST_CASE("passive insert never matches even when crossed") {
    OrderBook b;
    b.insert_order(1, Side::Buy, 100, 10);
    b.insert_order(2, Side::Sell, 100, 10);  // locked/crossed but NOT matched
    CHECK_EQ(b.order_count(), 2u);
    CHECK_EQ(b.best_bid().value(), 100);
    CHECK_EQ(b.best_ask().value(), 100);
}

TEST_CASE("reduce_order partial then to zero") {
    OrderBook b;
    b.insert_order(1, Side::Sell, 100, 10);
    CHECK(b.reduce_order(1, 4));
    CHECK_EQ(b.quantity_at(Side::Sell, 100), 6u);
    CHECK(b.reduce_order(1, 6));           // exact drain -> removes order
    CHECK(!b.contains(1));
    CHECK_EQ(b.ask_levels(), 0u);
}

TEST_CASE("reduce_order by more than remaining removes the order") {
    OrderBook b;
    b.insert_order(1, Side::Sell, 100, 10);
    CHECK(b.reduce_order(1, 999));
    CHECK(!b.contains(1));
}

TEST_CASE("find_order exposes side/price/qty") {
    OrderBook b;
    b.insert_order(7, Side::Buy, 100, 12);
    const Order* o = b.find_order(7);
    REQUIRE(o != nullptr);
    CHECK(o->side == Side::Buy);
    CHECK_EQ(o->price, 100);
    CHECK_EQ(o->quantity, 12u);
    CHECK(b.find_order(8) == nullptr);
}
