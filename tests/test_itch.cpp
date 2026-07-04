#include "ob/itch_parser.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

using namespace ob;

namespace {

// Minimal big-endian encoders mirroring the parser's readers.
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(x >> 8);
    v.push_back(x & 0xFF);
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(x >> 24);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back(x & 0xFF);
}
void put48(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 5; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xFF);
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xFF);
}

// Build an 'A' Add Order (36 bytes).
std::vector<std::uint8_t> add_order(std::uint16_t locate, std::uint64_t ref,
                                    char side, std::uint32_t shares,
                                    const char* sym8, std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('A');
    put16(m, locate);
    put16(m, 0);          // tracking number
    put48(m, 0);          // timestamp
    put64(m, ref);
    m.push_back(static_cast<std::uint8_t>(side));
    put32(m, shares);
    for (int i = 0; i < 8; ++i) m.push_back(sym8[i] ? sym8[i] : ' ');
    put32(m, price);
    return m;
}

std::vector<std::uint8_t> exec_order(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('E');
    put16(m, 1);
    put16(m, 0);
    put48(m, 0);
    put64(m, ref);
    put32(m, shares);
    put64(m, 0);          // match number
    return m;
}

std::vector<std::uint8_t> cancel_order(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('X');
    put16(m, 1);
    put16(m, 0);
    put48(m, 0);
    put64(m, ref);
    put32(m, shares);
    return m;
}

std::vector<std::uint8_t> delete_order(std::uint64_t ref) {
    std::vector<std::uint8_t> m;
    m.push_back('D');
    put16(m, 1);
    put16(m, 0);
    put48(m, 0);
    put64(m, ref);
    return m;
}

std::vector<std::uint8_t> replace_order(std::uint64_t old_ref,
                                        std::uint64_t new_ref,
                                        std::uint32_t shares,
                                        std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('U');
    put16(m, 1);
    put16(m, 0);
    put48(m, 0);
    put64(m, old_ref);
    put64(m, new_ref);
    put32(m, shares);
    put32(m, price);
    return m;
}

} // namespace

TEST_CASE("itch: add reconstructs book at correct price/side") {
    ItchReplayer r;
    auto m = add_order(1, 1001, 'B', 500, "AAPL", 1500000);  // $150.0000
    r.handle_message(m.data(), m.size());

    const OrderBook* book = r.book_for_locate(1);
    REQUIRE(book != nullptr);
    CHECK_EQ(book->best_bid().value(), 1500000);
    CHECK_EQ(book->quantity_at(Side::Buy, 1500000), 500u);
    CHECK_EQ(r.stats().adds, 1u);
}

TEST_CASE("itch: execute reduces shares, drains and removes at zero") {
    ItchReplayer r;
    auto a = add_order(1, 2001, 'S', 300, "MSFT", 4000000);
    auto e1 = exec_order(2001, 100);
    auto e2 = exec_order(2001, 200);          // fully drains
    r.handle_message(a.data(), a.size());
    r.handle_message(e1.data(), e1.size());
    const OrderBook* book = r.book_for_locate(1);
    REQUIRE(book != nullptr);
    CHECK_EQ(book->quantity_at(Side::Sell, 4000000), 200u);
    r.handle_message(e2.data(), e2.size());
    CHECK(!book->contains(2001));
    CHECK_EQ(r.stats().executes, 2u);
}

TEST_CASE("itch: cancel reduces, delete removes") {
    ItchReplayer r;
    auto a = add_order(1, 3001, 'B', 400, "TSLA", 2000000);
    auto x = cancel_order(3001, 150);
    auto d = delete_order(3001);
    r.handle_message(a.data(), a.size());
    r.handle_message(x.data(), x.size());
    const OrderBook* book = r.book_for_locate(1);
    REQUIRE(book != nullptr);
    CHECK_EQ(book->quantity_at(Side::Buy, 2000000), 250u);
    r.handle_message(d.data(), d.size());
    CHECK(!book->contains(3001));
    CHECK_EQ(r.stats().cancels, 1u);
    CHECK_EQ(r.stats().deletes, 1u);
}

TEST_CASE("itch: replace preserves side, moves price, reassigns ref") {
    ItchReplayer r;
    auto a = add_order(1, 4001, 'B', 100, "NVDA", 5000000);
    auto u = replace_order(4001, 4002, 250, 5010000);  // new ref, new price/qty
    r.handle_message(a.data(), a.size());
    r.handle_message(u.data(), u.size());
    const OrderBook* book = r.book_for_locate(1);
    REQUIRE(book != nullptr);
    CHECK(!book->contains(4001));               // old ref gone
    const Order* o = book->find_order(4002);
    REQUIRE(o != nullptr);
    CHECK(o->side == Side::Buy);                 // side preserved from old order
    CHECK_EQ(o->price, 5010000);
    CHECK_EQ(o->quantity, 250u);
    CHECK_EQ(r.stats().replaces, 1u);
}

TEST_CASE("itch: framed stream replay routes by stock locate") {
    // Two symbols interleaved, length-prefixed as in a BinaryFILE.
    std::vector<std::uint8_t> a1 = add_order(1, 5001, 'B', 100, "AAA", 1000000);
    std::vector<std::uint8_t> a2 = add_order(2, 5002, 'S', 200, "BBB", 2000000);

    std::string bytes;
    auto frame = [&](const std::vector<std::uint8_t>& m) {
        bytes.push_back(static_cast<char>((m.size() >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(m.size() & 0xFF));
        bytes.append(reinterpret_cast<const char*>(m.data()), m.size());
    };
    frame(a1);
    frame(a2);

    std::istringstream in(bytes, std::ios::binary);
    ItchReplayer r;
    auto stats = r.replay_stream(in);
    CHECK_EQ(stats.messages, 2u);
    CHECK_EQ(r.book_count(), 2u);
    CHECK_EQ(r.book_for_locate(1)->best_bid().value(), 1000000);
    CHECK_EQ(r.book_for_locate(2)->best_ask().value(), 2000000);
}

TEST_CASE("itch: unknown message types are skipped harmlessly") {
    ItchReplayer r;
    std::vector<std::uint8_t> sys = {'S', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'O'};
    r.handle_message(sys.data(), sys.size());   // System Event -- no book impact
    CHECK_EQ(r.book_count(), 0u);
    CHECK_EQ(r.stats().adds, 0u);
}
