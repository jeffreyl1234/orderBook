#include "ob/order_book.hpp"

#include <algorithm>
#include <utility>

namespace ob {

// --- Core matching primitive -------------------------------------------
//
// Walks the best price of the opposite book downward (bids) or upward (asks),
// consuming resting orders FIFO within each level. Stops when the incoming
// order is exhausted, the book empties, or the next level no longer crosses.
template <class Book>
void OrderBook::match_(Book& opp, Side taker_side, OrderId taker_id, Price limit,
                       bool is_market, Quantity& qty, std::vector<Trade>& out) {
    while (qty > 0 && !opp.empty()) {
        auto level_it = opp.begin();          // best price on the opposite side
        const Price level_price = level_it->first;

        // A limit crosses only if it is at least as aggressive as the level.
        // A market order crosses any price (it is willing to pay whatever).
        const bool crosses =
            is_market ||
            (taker_side == Side::Buy ? limit >= level_price : limit <= level_price);
        if (!crosses) break;

        Level& level = level_it->second;
        while (qty > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();  // oldest = time priority
            const Quantity exec = std::min(qty, resting.quantity);

            // Execution price is the *maker's* price, not the taker's limit.
            out.push_back(Trade{taker_id, resting.id, level_price, exec});

            qty                -= exec;
            resting.quantity   -= exec;
            level.total        -= exec;

            if (resting.quantity == 0) {
                index_.erase(resting.id);
                level.orders.pop_front();
            }
        }

        if (level.orders.empty())
            opp.erase(level_it);  // drop the now-empty price level
    }
}

// --- Aggressive operations ---------------------------------------------

void OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty,
                          std::vector<Trade>& out) {
    if (side == Side::Buy)
        match_(asks_, Side::Buy, id, price, /*is_market=*/false, qty, out);
    else
        match_(bids_, Side::Sell, id, price, /*is_market=*/false, qty, out);

    // Whatever did not trade rests on the book at the limit price.
    if (qty > 0)
        insert_order(id, side, price, qty);
}

void OrderBook::add_market(OrderId id, Side side, Quantity qty,
                           std::vector<Trade>& out) {
    if (side == Side::Buy)
        match_(asks_, Side::Buy, id, 0, /*is_market=*/true, qty, out);
    else
        match_(bids_, Side::Sell, id, 0, /*is_market=*/true, qty, out);
    // A market order never rests; any unfilled remainder is discarded.
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price,
                                        Quantity qty) {
    std::vector<Trade> out;
    add_limit(id, side, price, qty, out);
    return out;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Trade> out;
    add_market(id, side, qty, out);
    return out;
}

bool OrderBook::cancel(OrderId id) {
    return remove_order(id);
}

void OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                       std::vector<Trade>& out) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return;

    const Side     side     = ix->second.side;
    const Price    old_price = ix->second.price;
    const Quantity old_qty  = ix->second.it->quantity;

    if (new_qty == 0) {                       // degenerate modify == cancel
        remove_order(id);
        return;
    }

    if (new_price == old_price && new_qty <= old_qty) {
        // Pure downsize at the same price: keep time priority, mutate in place.
        reduce_order(id, old_qty - new_qty);
    } else {
        // Price change or size increase: lose priority (cancel + re-add), and
        // the re-add runs the matcher because the new price may cross.
        remove_order(id);
        add_limit(id, side, new_price, new_qty, out);
    }
}

// --- Passive primitives -------------------------------------------------

void OrderBook::insert_order(OrderId id, Side side, Price price, Quantity qty) {
    if (side == Side::Buy) {
        Level& level = bids_[price];
        level.orders.push_back(Order{id, side, price, qty});
        level.total += qty;
        index_[id] = Location{side, price, std::prev(level.orders.end())};
    } else {
        Level& level = asks_[price];
        level.orders.push_back(Order{id, side, price, qty});
        level.total += qty;
        index_[id] = Location{side, price, std::prev(level.orders.end())};
    }
}

bool OrderBook::reduce_order(OrderId id, Quantity by) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return false;

    Location& loc = ix->second;
    const Quantity remaining = loc.it->quantity;
    if (by >= remaining) {
        // Reducing by the whole (or more) is a full remove.
        return remove_order(id);
    }

    loc.it->quantity -= by;
    if (loc.side == Side::Buy)
        bids_.find(loc.price)->second.total -= by;
    else
        asks_.find(loc.price)->second.total -= by;
    return true;
}

bool OrderBook::remove_order(OrderId id) {
    auto ix = index_.find(id);
    if (ix == index_.end()) return false;

    const Location loc = ix->second;
    if (loc.side == Side::Buy) {
        auto lvl = bids_.find(loc.price);
        lvl->second.total -= loc.it->quantity;
        lvl->second.orders.erase(loc.it);
        if (lvl->second.orders.empty()) bids_.erase(lvl);
    } else {
        auto lvl = asks_.find(loc.price);
        lvl->second.total -= loc.it->quantity;
        lvl->second.orders.erase(loc.it);
        if (lvl->second.orders.empty()) asks_.erase(lvl);
    }
    index_.erase(ix);
    return true;
}

// --- Queries ------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    } else {
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.total;
    }
}

const Order* OrderBook::find_order(OrderId id) const {
    auto ix = index_.find(id);
    if (ix == index_.end()) return nullptr;
    return &(*ix->second.it);
}

} // namespace ob
