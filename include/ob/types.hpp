#pragma once

#include <cstdint>

namespace ob {

// --- Fundamental domain types -------------------------------------------
//
// Prices are stored as *integer ticks*, never floating point. Two reasons
// you can defend in an interview:
//   1. Price levels are keyed by exact equality. `0.1 + 0.2 != 0.3` in IEEE
//      754, so a double-keyed map would silently create phantom levels.
//   2. Real market data (NASDAQ ITCH) already encodes price as a fixed-point
//      integer (1/10000 of a dollar), so integers are the native form.
using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // fixed-point ticks (ITCH: 1e-4 dollars)
using Quantity = std::uint64_t;  // shares / contracts

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// An execution report. `price` is always the *resting* (maker) order's price,
// which is the defining rule of price-time priority: the aggressor trades at
// the price already advertised by the book, never at its own limit.
struct Trade {
    OrderId  taker_id;   // incoming aggressor order
    OrderId  maker_id;   // resting order that was hit
    Price    price;      // execution price (== maker's resting price)
    Quantity quantity;   // shares exchanged
};

} // namespace ob
