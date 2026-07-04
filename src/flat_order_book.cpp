#include "ob/flat_order_book.hpp"

#include <algorithm>
#include <cassert>

namespace ob {

FlatOrderBook::FlatOrderBook(Price min_price, Price max_price, Price tick,
                             std::size_t capacity_hint)
    : min_(min_price), max_(max_price), tick_(tick) {
    assert(tick_ > 0 && max_ >= min_);
    num_levels_ = (max_ - min_) / tick_ + 1;
    bids_.assign(static_cast<std::size_t>(num_levels_), FlatLevel{});
    asks_.assign(static_cast<std::size_t>(num_levels_), FlatLevel{});
    if (capacity_hint) pool_.reserve(capacity_hint);
}

// --- Arena management ---------------------------------------------------

std::int32_t FlatOrderBook::alloc_node() {
    if (free_head_ != -1) {
        const std::int32_t i = free_head_;
        free_head_ = pool_[i].next;   // next doubles as the free-list link
        return i;
    }
    pool_.push_back(Node{});
    return static_cast<std::int32_t>(pool_.size() - 1);
}

void FlatOrderBook::free_node(std::int32_t i) {
    pool_[i].next = free_head_;
    free_head_ = i;
}

// --- Intrusive FIFO wiring ----------------------------------------------

void FlatOrderBook::push_back(FlatLevel& lvl, std::int32_t ni) {
    Node& n = pool_[ni];
    n.next = -1;
    n.prev = lvl.tail;
    if (lvl.tail != -1)
        pool_[lvl.tail].next = ni;
    else
        lvl.head = ni;
    lvl.tail = ni;
    lvl.total += n.ord.quantity;
}

void FlatOrderBook::unlink(FlatLevel& lvl, std::int32_t ni) {
    Node& n = pool_[ni];
    if (n.prev != -1) pool_[n.prev].next = n.next; else lvl.head = n.next;
    if (n.next != -1) pool_[n.next].prev = n.prev; else lvl.tail = n.prev;
    lvl.total -= n.ord.quantity;
}

// Find the next occupied level after the best one empties. Bounded by the price
// range; monotonic within a match sweep, so amortized cheap in practice.
void FlatOrderBook::rescan_bid_down(std::int64_t from) {
    for (std::int64_t j = from - 1; j >= 0; --j)
        if (bids_[j].head != -1) { best_bid_idx_ = j; return; }
    best_bid_idx_ = -1;
}

void FlatOrderBook::rescan_ask_up(std::int64_t from) {
    for (std::int64_t j = from + 1; j < num_levels_; ++j)
        if (asks_[j].head != -1) { best_ask_idx_ = j; return; }
    best_ask_idx_ = -1;
}

// --- Matching -----------------------------------------------------------

template <bool BuyTaker>
void FlatOrderBook::match_(OrderId taker_id, std::int64_t limit_idx,
                           bool is_market, Quantity& qty,
                           std::vector<Trade>& out) {
    auto& best = BuyTaker ? best_ask_idx_ : best_bid_idx_;
    auto& levels = BuyTaker ? asks_ : bids_;
    auto& count = BuyTaker ? ask_count_ : bid_count_;

    while (qty > 0 && best != -1) {
        // A buy crosses asks at index <= its limit; a sell crosses bids at
        // index >= its limit. Market orders cross anything.
        if (!is_market) {
            if (BuyTaker ? (best > limit_idx) : (best < limit_idx)) break;
        }
        const std::int64_t idx = best;
        FlatLevel& lvl = levels[static_cast<std::size_t>(idx)];
        const Price px = to_price(idx);

        while (qty > 0 && lvl.head != -1) {
            const std::int32_t hi = lvl.head;
            Node& resting = pool_[hi];
            const Quantity exec = std::min(qty, resting.ord.quantity);

            out.push_back(Trade{taker_id, resting.ord.id, px, exec});
            qty                  -= exec;
            resting.ord.quantity -= exec;
            lvl.total            -= exec;

            if (resting.ord.quantity == 0) {
                index_.erase(resting.ord.id);
                const std::int32_t nx = resting.next;
                lvl.head = nx;
                if (nx != -1) pool_[nx].prev = -1; else lvl.tail = -1;
                free_node(hi);
                --order_count_;
            }
        }

        if (lvl.head == -1) {
            --count;
            if (BuyTaker) rescan_ask_up(idx); else rescan_bid_down(idx);
        }
    }
}

// --- Aggressive operations ---------------------------------------------

void FlatOrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty,
                              std::vector<Trade>& out) {
    const std::int64_t lim = to_idx(price);
    if (side == Side::Buy)
        match_<true>(id, lim, /*is_market=*/false, qty, out);
    else
        match_<false>(id, lim, /*is_market=*/false, qty, out);

    if (qty > 0) insert_order(id, side, price, qty);
}

void FlatOrderBook::add_market(OrderId id, Side side, Quantity qty,
                               std::vector<Trade>& out) {
    if (side == Side::Buy)
        match_<true>(id, 0, /*is_market=*/true, qty, out);
    else
        match_<false>(id, 0, /*is_market=*/true, qty, out);
}

std::vector<Trade> FlatOrderBook::add_limit(OrderId id, Side side, Price price,
                                            Quantity qty) {
    std::vector<Trade> out;
    add_limit(id, side, price, qty, out);
    return out;
}

std::vector<Trade> FlatOrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Trade> out;
    add_market(id, side, qty, out);
    return out;
}

bool FlatOrderBook::cancel(OrderId id) { return remove_order(id); }

void FlatOrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                           std::vector<Trade>& out) {
    auto it = index_.find(id);
    if (it == index_.end()) return;

    const Node& n = pool_[it->second];
    const Side     side = n.ord.side;
    const Price    op   = n.ord.price;
    const Quantity oq   = n.ord.quantity;

    if (new_qty == 0) { remove_order(id); return; }
    if (new_price == op && new_qty <= oq) {
        reduce_order(id, oq - new_qty);            // keep priority
    } else {
        remove_order(id);
        add_limit(id, side, new_price, new_qty, out);  // lose priority, may cross
    }
}

// --- Passive primitives -------------------------------------------------

void FlatOrderBook::insert_order(OrderId id, Side side, Price price,
                                 Quantity qty) {
    assert(in_range(price) && "price outside the flat book's configured range");
    const std::int64_t idx = to_idx(price);
    const std::int32_t ni = alloc_node();
    pool_[ni].ord = Order{id, side, price, qty};
    pool_[ni].lvl = static_cast<std::int32_t>(idx);

    if (side == Side::Buy) {
        FlatLevel& lvl = bids_[static_cast<std::size_t>(idx)];
        if (lvl.head == -1) ++bid_count_;
        push_back(lvl, ni);
        if (best_bid_idx_ == -1 || idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        FlatLevel& lvl = asks_[static_cast<std::size_t>(idx)];
        if (lvl.head == -1) ++ask_count_;
        push_back(lvl, ni);
        if (best_ask_idx_ == -1 || idx < best_ask_idx_) best_ask_idx_ = idx;
    }
    index_[id] = ni;
    ++order_count_;
}

bool FlatOrderBook::reduce_order(OrderId id, Quantity by) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Node& n = pool_[it->second];
    if (by >= n.ord.quantity) return remove_order(id);
    n.ord.quantity -= by;
    (n.ord.side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(n.lvl)]
        .total -= by;
    return true;
}

bool FlatOrderBook::remove_order(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const std::int32_t ni = it->second;
    const std::int64_t idx = pool_[ni].lvl;
    const Side side = pool_[ni].ord.side;

    FlatLevel& lvl =
        (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(idx)];
    unlink(lvl, ni);
    if (lvl.head == -1) {
        if (side == Side::Buy) {
            --bid_count_;
            if (idx == best_bid_idx_) rescan_bid_down(idx);
        } else {
            --ask_count_;
            if (idx == best_ask_idx_) rescan_ask_up(idx);
        }
    }
    free_node(ni);
    index_.erase(it);
    --order_count_;
    return true;
}

// --- Queries ------------------------------------------------------------

std::optional<Price> FlatOrderBook::best_bid() const {
    if (best_bid_idx_ == -1) return std::nullopt;
    return to_price(best_bid_idx_);
}

std::optional<Price> FlatOrderBook::best_ask() const {
    if (best_ask_idx_ == -1) return std::nullopt;
    return to_price(best_ask_idx_);
}

Quantity FlatOrderBook::quantity_at(Side side, Price price) const {
    if (!in_range(price)) return 0;
    const std::int64_t idx = to_idx(price);
    return (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(idx)].total;
}

const Order* FlatOrderBook::find_order(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &pool_[it->second].ord;
}

} // namespace ob
