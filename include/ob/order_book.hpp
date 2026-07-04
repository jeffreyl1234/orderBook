#pragma once

#include "ob/order.hpp"
#include "ob/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

// A price level: FIFO queue of resting orders plus a running total so that
// depth queries are O(1) instead of O(orders-at-level).
//
// `std::list` is chosen deliberately: its iterators are *stable* across
// insertion/erasure of other elements. We stash each order's iterator in the
// order index, which turns cancel-by-id into an O(1) splice-out instead of a
// linear scan. The per-node allocation is the cost we pay in v1; the flat-array
// iteration (v2) is where we attack it.
struct Level {
    std::list<Order> orders;   // front() == oldest == highest time priority
    Quantity         total = 0; // sum of remaining quantity at this level
};

// Single-threaded limit order book + matching engine with price-time priority.
//
// Two families of operations share one set of primitives:
//   * Aggressive (add_limit / add_market / modify) run the matcher.
//   * Passive (insert_order / reduce_order / remove_order) mutate the book
//     directly with NO matching. These are what an ITCH replay drives: the
//     exchange already decided the matches, so we just reconstruct state.
class OrderBook {
public:
    OrderBook() = default;

    // --- Aggressive / matching operations -------------------------------
    // Trades are appended to `out` (caller-reused to keep the hot path
    // allocation-free — important for the benchmark).
    void add_limit(OrderId id, Side side, Price price, Quantity qty,
                   std::vector<Trade>& out);
    void add_market(OrderId id, Side side, Quantity qty,
                    std::vector<Trade>& out);

    // Convenience overloads that allocate a fresh vector (tests/readability).
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty);

    // Cancel a resting order. Returns false if the id is unknown.
    bool cancel(OrderId id);

    // Modify semantics follow NASDAQ price-time rules:
    //   * pure size *decrease* at the same price keeps time priority (in place).
    //   * a price change or size *increase* is a cancel + re-add: it loses time
    //     priority and may cross the book (hence the `out` trades).
    void modify(OrderId id, Price new_price, Quantity new_qty,
                std::vector<Trade>& out);

    // --- Passive operations (feed replay, no matching) ------------------
    void insert_order(OrderId id, Side side, Price price, Quantity qty);
    bool reduce_order(OrderId id, Quantity by);  // execute / partial cancel
    bool remove_order(OrderId id);               // full delete

    // --- Queries --------------------------------------------------------
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity quantity_at(Side side, Price price) const;
    const Order* find_order(OrderId id) const;   // nullptr if unknown

    std::size_t order_count() const { return index_.size(); }
    std::size_t bid_levels()  const { return bids_.size(); }
    std::size_t ask_levels()  const { return asks_.size(); }
    bool contains(OrderId id) const { return index_.find(id) != index_.end(); }
    bool empty() const { return index_.empty(); }

private:
    // Where a live order lives, for O(1) lookup/cancel.
    struct Location {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator it;
    };

    // Bids sort high->low, asks low->high, so begin() is always the best price.
    using BidBook = std::map<Price, Level, std::greater<Price>>;
    using AskBook = std::map<Price, Level, std::less<Price>>;

    // Sweep `qty` of an incoming order against the opposite book, price-time
    // ordered, appending trades. Templated on book type so bids/asks share one
    // implementation (they differ only in comparator). Defined in the .cpp.
    template <class Book>
    void match_(Book& opp, Side taker_side, OrderId taker_id, Price limit,
                bool is_market, Quantity& qty, std::vector<Trade>& out);

    BidBook bids_;
    AskBook asks_;
    std::unordered_map<OrderId, Location> index_;
};

} // namespace ob
