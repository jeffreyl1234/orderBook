#pragma once

#include "ob/order.hpp"
#include "ob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

// v2: cache-friendly limit order book, drop-in API-compatible with OrderBook.
//
// Where v1 (OrderBook) uses std::map<Price,Level> + std::list<Order> — node
// containers that malloc per order and chase pointers all over the heap — v2
// attacks all three costs:
//
//   1. Price levels are a *flat array* indexed by (price - min) / tick, so best-
//      /arbitrary-level access is O(1) array indexing instead of O(log L) tree
//      descent, and neighbouring levels are contiguous in memory.
//   2. Orders live in a contiguous *arena* (std::vector<Node>) with an intrusive
//      free list, so there is no per-order heap allocation and consuming a level
//      FIFO walks contiguous-ish memory.
//   3. The FIFO queues are intrusive singly/doubly linked lists threaded through
//      the arena by 32-bit indices (not pointers), keeping nodes compact.
//
// The tradeoff — and the reason v1 is kept for the multi-symbol ITCH replay — is
// that a flat array must preallocate the whole [min,max] price range, so it fits
// a single instrument trading in a bounded intraday band, not thousands of
// symbols with wildly different prices. Best-price is tracked incrementally with
// an O(1) update on insert and a bounded rescan only when the top level empties.
class FlatOrderBook {
public:
    // `capacity_hint` pre-reserves the order arena to avoid reallocation churn.
    FlatOrderBook(Price min_price, Price max_price, Price tick = 1,
                  std::size_t capacity_hint = 0);

    // --- Aggressive / matching (identical semantics to OrderBook) --------
    void add_limit(OrderId id, Side side, Price price, Quantity qty,
                   std::vector<Trade>& out);
    void add_market(OrderId id, Side side, Quantity qty,
                    std::vector<Trade>& out);
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty);
    bool cancel(OrderId id);
    void modify(OrderId id, Price new_price, Quantity new_qty,
                std::vector<Trade>& out);

    // --- Passive primitives ---------------------------------------------
    void insert_order(OrderId id, Side side, Price price, Quantity qty);
    bool reduce_order(OrderId id, Quantity by);
    bool remove_order(OrderId id);

    // --- Queries --------------------------------------------------------
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity quantity_at(Side side, Price price) const;
    // NOTE: the returned pointer is valid only until the next mutating call
    // (the arena may reallocate). v1 offers a stronger guarantee; this is the
    // documented cost of a contiguous arena.
    const Order* find_order(OrderId id) const;

    std::size_t order_count() const { return order_count_; }
    std::size_t bid_levels()  const { return bid_count_; }
    std::size_t ask_levels()  const { return ask_count_; }
    bool contains(OrderId id) const { return index_.find(id) != index_.end(); }
    bool empty() const { return order_count_ == 0; }
    bool in_range(Price price) const {
        return price >= min_ && price <= max_;
    }

private:
    // One order in the arena. `next`/`prev` index into `pool_` (‑1 == none) and
    // form the level's FIFO; when a node is freed, `next` doubles as the free-
    // list link. `lvl` caches the price index for O(1) cancel bookkeeping.
    struct Node {
        Order   ord;
        std::int32_t next = -1;
        std::int32_t prev = -1;
        std::int32_t lvl  = -1;
    };
    struct FlatLevel {
        std::int32_t head = -1;  // oldest (FIFO front)
        std::int32_t tail = -1;  // newest
        Quantity     total = 0;
    };

    std::int64_t to_idx(Price p) const { return (p - min_) / tick_; }
    Price        to_price(std::int64_t i) const {
        return min_ + static_cast<Price>(i) * tick_;
    }

    std::int32_t alloc_node();
    void         free_node(std::int32_t i);
    void         push_back(FlatLevel& lvl, std::int32_t ni);
    void         unlink(FlatLevel& lvl, std::int32_t ni);
    void         rescan_bid_down(std::int64_t from);
    void         rescan_ask_up(std::int64_t from);

    template <bool BuyTaker>
    void match_(OrderId taker_id, std::int64_t limit_idx, bool is_market,
                Quantity& qty, std::vector<Trade>& out);

    Price       min_, max_, tick_;
    std::int64_t num_levels_;

    std::vector<Node>      pool_;
    std::int32_t           free_head_ = -1;
    std::vector<FlatLevel> bids_;
    std::vector<FlatLevel> asks_;
    std::unordered_map<OrderId, std::int32_t> index_;

    std::int64_t best_bid_idx_ = -1;  // highest occupied bid index (‑1 empty)
    std::int64_t best_ask_idx_ = -1;  // lowest  occupied ask index (‑1 empty)
    std::size_t  bid_count_ = 0, ask_count_ = 0, order_count_ = 0;
};

} // namespace ob
