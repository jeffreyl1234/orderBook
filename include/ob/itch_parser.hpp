#pragma once

#include "ob/order_book.hpp"
#include "ob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>

namespace ob {

// Counters accumulated while replaying a feed, for a summary report.
struct ReplayStats {
    std::uint64_t messages = 0;   // total framed messages seen
    std::uint64_t adds     = 0;   // 'A' / 'F'
    std::uint64_t executes = 0;   // 'E' / 'C'
    std::uint64_t cancels  = 0;   // 'X'
    std::uint64_t deletes  = 0;   // 'D'
    std::uint64_t replaces = 0;   // 'U'
    std::uint64_t trades   = 0;   // executions that moved shares
};

// NASDAQ TotalView-ITCH 5.0 replayer.
//
// The public sample files use the "BinaryFILE" framing: each message is
// preceded by a 2-byte big-endian length, so we never need to hard-code the
// per-type message sizes just to advance — the length prefix does that, and
// unknown message types are skipped for free.
//
// Book reconstruction is intentionally a *literal* replay: the exchange has
// already matched, so we drive the OrderBook's passive primitives, never the
// matcher. Order Reference Numbers are globally unique for the day and are used
// directly as our OrderId. Execution/cancel/delete/replace messages carry only
// the reference number, so we keep a ref -> stock-locate index to route them to
// the right per-symbol book.
class ItchReplayer {
public:
    // Feed one already-framed message (length-prefix stripped). `len` is the
    // message body length; `data[0]` is the message type character.
    void handle_message(const std::uint8_t* data, std::size_t len);

    // Stream a BinaryFILE (2-byte length-prefixed messages) to exhaustion.
    ReplayStats replay_stream(std::istream& in);

    const ReplayStats& stats() const { return stats_; }
    const OrderBook*   book_for_locate(std::uint16_t locate) const;
    std::size_t        book_count() const { return books_.size(); }

    // Exposed for the summary report: locate -> symbol (from 'R' messages).
    const std::unordered_map<std::uint16_t, std::string>& symbols() const {
        return symbols_;
    }
    const std::unordered_map<std::uint16_t, OrderBook>& books() const {
        return books_;
    }

private:
    std::unordered_map<std::uint16_t, OrderBook>   books_;        // per stock
    std::unordered_map<std::uint64_t, std::uint16_t> ref_locate_; // route by ref
    std::unordered_map<std::uint16_t, std::string> symbols_;      // locate->sym
    ReplayStats stats_;
};

} // namespace ob
