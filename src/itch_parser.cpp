#include "ob/itch_parser.hpp"

#include <array>
#include <vector>

namespace ob {
namespace {

// ITCH is big-endian ("network byte order"). These read fixed-width fields
// without any unaligned-load or aliasing UB.
inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint32_t(p[0]) << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Message body lengths (including the 1-byte type) for the types we decode.
// Used only as bounds checks so a truncated/garbage message can't over-read.
constexpr std::size_t kLenAddNoMPID = 36; // 'A'
constexpr std::size_t kLenAddMPID   = 40; // 'F'
constexpr std::size_t kLenExec      = 31; // 'E'
constexpr std::size_t kLenExecPrice = 36; // 'C'
constexpr std::size_t kLenCancel    = 23; // 'X'
constexpr std::size_t kLenDelete    = 19; // 'D'
constexpr std::size_t kLenReplace   = 35; // 'U'
constexpr std::size_t kLenStockDir  = 39; // 'R'

} // namespace

void ItchReplayer::handle_message(const std::uint8_t* d, std::size_t len) {
    if (len == 0) return;
    const char type = static_cast<char>(d[0]);

    switch (type) {
        case 'R': { // Stock Directory: record locate -> symbol for the report.
            if (len < kLenStockDir) return;
            const std::uint16_t locate = be16(d + 1);
            std::string sym(reinterpret_cast<const char*>(d + 11), 8);
            // Symbols are space-padded to 8 chars; trim trailing spaces.
            const auto end = sym.find_last_not_of(' ');
            sym = end == std::string::npos ? std::string{} : sym.substr(0, end + 1);
            symbols_[locate] = std::move(sym);
            break;
        }
        case 'A':   // Add Order (no MPID)
        case 'F': { // Add Order with MPID -- identical layout for our fields
            if (len < (type == 'A' ? kLenAddNoMPID : kLenAddMPID)) return;
            const std::uint16_t locate = be16(d + 1);
            const std::uint64_t ref    = be64(d + 11);
            const Side side = (d[19] == 'B') ? Side::Buy : Side::Sell;
            const Quantity shares = be32(d + 20);
            const Price    price  = static_cast<Price>(be32(d + 32));

            books_[locate].insert_order(ref, side, price, shares);
            ref_locate_[ref] = locate;
            ++stats_.adds;
            break;
        }
        case 'E': { // Order Executed: reduce resting order by executed shares.
            if (len < kLenExec) return;
            const std::uint64_t ref  = be64(d + 11);
            const Quantity      exec = be32(d + 19);
            auto rl = ref_locate_.find(ref);
            if (rl != ref_locate_.end()) {
                OrderBook& book = books_[rl->second];
                book.reduce_order(ref, exec);
                if (!book.contains(ref)) ref_locate_.erase(rl);
            }
            ++stats_.executes;
            ++stats_.trades;
            break;
        }
        case 'C': { // Order Executed With Price (e.g. price improvement).
            if (len < kLenExecPrice) return;
            const std::uint64_t ref  = be64(d + 11);
            const Quantity      exec = be32(d + 19);
            auto rl = ref_locate_.find(ref);
            if (rl != ref_locate_.end()) {
                OrderBook& book = books_[rl->second];
                book.reduce_order(ref, exec);
                if (!book.contains(ref)) ref_locate_.erase(rl);
            }
            ++stats_.executes;
            ++stats_.trades;
            break;
        }
        case 'X': { // Order Cancel: partial cancel of `canceled` shares.
            if (len < kLenCancel) return;
            const std::uint64_t ref      = be64(d + 11);
            const Quantity      canceled = be32(d + 19);
            auto rl = ref_locate_.find(ref);
            if (rl != ref_locate_.end()) {
                OrderBook& book = books_[rl->second];
                book.reduce_order(ref, canceled);
                if (!book.contains(ref)) ref_locate_.erase(rl);
            }
            ++stats_.cancels;
            break;
        }
        case 'D': { // Order Delete: remove the order entirely.
            if (len < kLenDelete) return;
            const std::uint64_t ref = be64(d + 11);
            auto rl = ref_locate_.find(ref);
            if (rl != ref_locate_.end()) {
                books_[rl->second].remove_order(ref);
                ref_locate_.erase(rl);
            }
            ++stats_.deletes;
            break;
        }
        case 'U': { // Order Replace: cancel old ref, add new ref (loses priority).
            if (len < kLenReplace) return;
            const std::uint64_t old_ref = be64(d + 11);
            const std::uint64_t new_ref = be64(d + 19);
            const Quantity      shares  = be32(d + 27);
            const Price         price   = static_cast<Price>(be32(d + 31));

            auto rl = ref_locate_.find(old_ref);
            if (rl != ref_locate_.end()) {
                const std::uint16_t locate = rl->second;
                OrderBook& book = books_[locate];
                Side side = Side::Buy;
                if (const Order* o = book.find_order(old_ref)) side = o->side;
                book.remove_order(old_ref);
                book.insert_order(new_ref, side, price, shares);
                ref_locate_.erase(rl);
                ref_locate_[new_ref] = locate;
            }
            ++stats_.replaces;
            break;
        }
        default:
            break; // System event, trades, NOII, etc. -- no book impact.
    }
}

ReplayStats ItchReplayer::replay_stream(std::istream& in) {
    std::array<char, 2> hdr{};
    std::vector<std::uint8_t> buf;
    while (in.read(hdr.data(), 2)) {
        const std::uint16_t len =
            static_cast<std::uint16_t>((std::uint8_t(hdr[0]) << 8) | std::uint8_t(hdr[1]));
        if (len == 0) continue;
        buf.resize(len);
        if (!in.read(reinterpret_cast<char*>(buf.data()), len)) break;
        handle_message(buf.data(), len);
        ++stats_.messages;
    }
    return stats_;
}

const OrderBook* ItchReplayer::book_for_locate(std::uint16_t locate) const {
    auto it = books_.find(locate);
    return it == books_.end() ? nullptr : &it->second;
}

} // namespace ob
