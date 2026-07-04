#include "ob/flat_order_book.hpp"
#include "test_framework.hpp"

using namespace ob;

// The v2 flat book must be behaviourally identical to v1. These mirror the core
// cases from test_matching.cpp against FlatOrderBook to prove the drop-in holds.

TEST_CASE("flat: non-crossing rests, best bid/ask correct") {
    FlatOrderBook b(90, 110);
    auto t1 = b.add_limit(1, Side::Buy, 100, 10);
    auto t2 = b.add_limit(2, Side::Sell, 101, 10);
    CHECK(t1.empty());
    CHECK(t2.empty());
    CHECK_EQ(b.best_bid().value(), 100);
    CHECK_EQ(b.best_ask().value(), 101);
    CHECK_EQ(b.order_count(), 2u);
}

TEST_CASE("flat: empty book has no best bid/ask") {
    FlatOrderBook b(90, 110);
    CHECK(!b.best_bid().has_value());
    CHECK(!b.best_ask().has_value());
    CHECK(b.empty());
}

TEST_CASE("flat: exact full fill removes both") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 1u);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK(b.empty());
}

TEST_CASE("flat: partial fill leaves remainder resting") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 100, 25);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK(!b.best_ask().has_value());
    CHECK_EQ(b.quantity_at(Side::Buy, 100), 15u);
}

TEST_CASE("flat: buy above ask trades at ask price") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 105, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].price, 100);
}

TEST_CASE("flat: time priority FIFO at a level") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 100, 10);
    auto trades = b.add_limit(3, Side::Buy, 100, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 1u);
    CHECK(b.contains(2));
    CHECK(!b.contains(1));
}

TEST_CASE("flat: price priority, better level first") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 102, 10);
    b.add_limit(2, Side::Sell, 100, 10);
    b.add_limit(3, Side::Sell, 101, 10);
    auto trades = b.add_limit(4, Side::Buy, 105, 10);
    REQUIRE(trades.size() == 1);
    CHECK_EQ(trades[0].maker_id, 2u);
    CHECK_EQ(trades[0].price, 100);
}

TEST_CASE("flat: aggressor sweeps multiple levels in order") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 101, 10);
    b.add_limit(3, Side::Sell, 102, 10);
    auto trades = b.add_limit(4, Side::Buy, 102, 25);
    REQUIRE(trades.size() == 3);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[1].price, 101);
    CHECK_EQ(trades[2].price, 102);
    CHECK_EQ(trades[2].quantity, 5u);
    CHECK_EQ(b.quantity_at(Side::Sell, 102), 5u);
    CHECK_EQ(b.best_ask().value(), 102);
}

TEST_CASE("flat: sell aggressor sweeps bids downward") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Buy, 100, 10);
    b.add_limit(2, Side::Buy, 99, 10);
    b.add_limit(3, Side::Buy, 98, 10);
    auto trades = b.add_limit(4, Side::Sell, 98, 25);
    REQUIRE(trades.size() == 3);
    CHECK_EQ(trades[0].price, 100);   // best bid first
    CHECK_EQ(trades[1].price, 99);
    CHECK_EQ(trades[2].price, 98);
    CHECK_EQ(b.best_bid().value(), 98);
}

TEST_CASE("flat: market order sweeps regardless of price, discards remainder") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Sell, 100, 10);
    b.add_limit(2, Side::Sell, 105, 10);
    auto trades = b.add_market(3, Side::Buy, 15);
    REQUIRE(trades.size() == 2);
    CHECK_EQ(trades[1].price, 105);
    CHECK_EQ(trades[1].quantity, 5u);
    auto none = b.add_market(4, Side::Buy, 100);  // drains rest
    CHECK_EQ(none.size(), 1u);
    CHECK(b.empty());
}

TEST_CASE("flat: cancel tears down empty level and updates best") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Buy, 100, 10);
    b.add_limit(2, Side::Buy, 99, 10);
    CHECK_EQ(b.best_bid().value(), 100);
    CHECK(b.cancel(1));
    CHECK_EQ(b.bid_levels(), 1u);
    CHECK_EQ(b.best_bid().value(), 99);   // best rescanned down to 99
    CHECK(!b.cancel(999));
}

TEST_CASE("flat: modify downsize keeps priority, reprice loses it") {
    FlatOrderBook b(90, 110);
    b.add_limit(1, Side::Buy, 100, 20);
    b.add_limit(2, Side::Buy, 100, 20);
    std::vector<Trade> out;
    b.modify(1, 100, 5, out);
    CHECK(out.empty());
    CHECK_EQ(b.quantity_at(Side::Buy, 100), 25u);
    auto t = b.add_limit(3, Side::Sell, 100, 5);
    REQUIRE(t.size() == 1);
    CHECK_EQ(t[0].maker_id, 1u);          // order 1 kept its place

    b.add_limit(4, Side::Sell, 105, 10);
    b.modify(2, 105, 10, out);            // reprice up -> crosses the ask
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0].maker_id, 4u);
    CHECK_EQ(out[0].price, 105);
}

TEST_CASE("flat: arena reuse across many insert/cancel cycles stays correct") {
    FlatOrderBook b(0, 1000, 1, /*capacity_hint=*/8);
    // Churn far more orders than the reserved capacity to exercise the free list.
    for (int cycle = 0; cycle < 50; ++cycle) {
        for (int i = 0; i < 20; ++i)
            b.insert_order(static_cast<OrderId>(i), Side::Buy, 500 + i, 10);
        CHECK_EQ(b.order_count(), 20u);
        for (int i = 0; i < 20; ++i) CHECK(b.cancel(static_cast<OrderId>(i)));
        CHECK(b.empty());
        CHECK(!b.best_bid().has_value());
    }
}
