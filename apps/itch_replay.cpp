#include "ob/itch_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ob;

// Replays a NASDAQ TotalView-ITCH 5.0 BinaryFILE and prints a reconstruction
// summary. Reads from a path, or from stdin when given "-", so gzip'd samples
// can be streamed without linking a decompression library:
//
//     gunzip -c 01302019.NASDAQ_ITCH50.gz | ./itch_replay -
//     ./itch_replay 01302019.NASDAQ_ITCH50
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <itch_file|-> [top_n_symbols]\n"
                     "  reads a 2-byte length-prefixed ITCH 5.0 file;\n"
                     "  use '-' to read from stdin (e.g. via gunzip -c).\n",
                     argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const std::size_t top_n = argc > 2 ? std::stoul(argv[2]) : 10;

    ItchReplayer r;
    if (path == "-") {
        r.replay_stream(std::cin);
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
            return 1;
        }
        r.replay_stream(in);
    }

    const ReplayStats& s = r.stats();
    std::printf("=== ITCH 5.0 replay summary ===\n");
    std::printf("messages : %llu\n", (unsigned long long)s.messages);
    std::printf("adds     : %llu\n", (unsigned long long)s.adds);
    std::printf("executes : %llu\n", (unsigned long long)s.executes);
    std::printf("cancels  : %llu\n", (unsigned long long)s.cancels);
    std::printf("deletes  : %llu\n", (unsigned long long)s.deletes);
    std::printf("replaces : %llu\n", (unsigned long long)s.replaces);
    std::printf("books    : %zu symbols with reconstructed state\n\n",
                r.book_count());

    // Rank the still-populated books by resting order count and show top-of-book.
    struct Row {
        std::uint16_t locate;
        std::size_t   orders;
    };
    std::vector<Row> rows;
    for (const auto& [locate, book] : r.books())
        if (!book.empty()) rows.push_back({locate, book.order_count()});
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.orders > b.orders; });

    std::printf("%-10s %-14s %-14s %-10s\n", "symbol", "bid", "ask", "orders");
    const auto& syms = r.symbols();
    for (std::size_t i = 0; i < rows.size() && i < top_n; ++i) {
        const OrderBook& book = *r.book_for_locate(rows[i].locate);
        auto sit = syms.find(rows[i].locate);
        const std::string sym =
            sit != syms.end() ? sit->second : ("#" + std::to_string(rows[i].locate));
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        std::printf("%-10s %-14s %-14s %-10zu\n", sym.c_str(),
                    bid ? std::to_string(*bid).c_str() : "-",
                    ask ? std::to_string(*ask).c_str() : "-", rows[i].orders);
    }
    return 0;
}
