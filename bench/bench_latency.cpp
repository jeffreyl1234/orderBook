#include "ob/order_book.hpp"
#include "ob/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace ob;

namespace {

// Percentiles by nearest-rank on a sorted copy. Exact (not a streaming
// histogram) because we can afford to keep every sample for an offline bench;
// keeps the numbers trustworthy for the p999 tail we actually care about.
struct Stats {
    std::uint64_t p50, p99, p999, min, max;
    double mean;
    std::size_t n;
};

Stats summarize(std::vector<std::uint64_t> s) {
    Stats out{};
    out.n = s.size();
    if (s.empty()) return out;
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(p * (s.size() - 1) + 0.5);
        return s[idx];
    };
    out.p50 = pct(0.50);
    out.p99 = pct(0.99);
    out.p999 = pct(0.999);
    out.min = s.front();
    out.max = s.back();
    double sum = 0;
    for (auto v : s) sum += static_cast<double>(v);
    out.mean = sum / s.size();
    return out;
}

void print_row(const char* name, const Stats& s) {
    std::printf("%-16s %10zu %8llu %8.1f %8llu %8llu %8llu %8llu\n", name, s.n,
                (unsigned long long)s.min, s.mean, (unsigned long long)s.p50,
                (unsigned long long)s.p99, (unsigned long long)s.p999,
                (unsigned long long)s.max);
}

// Measure the floor: how long `now_ns()` itself takes, so a reader can tell
// signal from clock overhead. Reported alongside the results, never subtracted
// (subtracting would be dishonest at these magnitudes).
std::uint64_t measure_clock_overhead(int iters) {
    std::vector<std::uint64_t> d;
    d.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        const std::uint64_t a = now_ns();
        const std::uint64_t b = now_ns();
        d.push_back(b - a);
    }
    return summarize(std::move(d)).p50;
}

} // namespace

int main(int argc, char** argv) {
    int N = 200000;                 // operations timed per scenario
    if (argc > 1) N = std::atoi(argv[1]);
    const int warmup = N / 10;      // 10% warm-up, discarded

    std::printf("Order book latency benchmark  (single-threaded)\n");
    std::printf("clock: steady_clock/now_ns    ops/scenario: %d    warm-up: %d\n",
                N, warmup);
    std::printf("clock overhead (p50 of back-to-back now_ns): %llu ns\n\n",
                (unsigned long long)measure_clock_overhead(50000));

    std::printf("%-16s %10s %8s %8s %8s %8s %8s %8s\n", "scenario", "count",
                "min", "mean", "p50", "p99", "p999", "max");
    std::printf("%-16s %10s %8s %8s %8s %8s %8s %8s\n", "(ns)", "", "", "", "",
                "", "", "");

    std::mt19937_64 rng(12345);

    // --- ADD: non-crossing inserts (pure resting-order insert path) --------
    {
        OrderBook book;
        std::vector<Trade> trades;
        trades.reserve(4);
        // Warm-up inserts at low prices, discarded.
        for (int i = 0; i < warmup; ++i)
            book.add_limit(static_cast<OrderId>(i), Side::Buy, 1000 + (i % 500),
                           10, trades);

        std::vector<std::uint64_t> samples;
        samples.reserve(N);
        for (int i = 0; i < N; ++i) {
            const OrderId id = static_cast<OrderId>(warmup + i);
            const Price px = 2000 + (i % 1000);  // spread across 1000 levels
            const std::uint64_t t0 = now_ns();
            book.add_limit(id, Side::Buy, px, 10, trades);
            const std::uint64_t t1 = now_ns();
            samples.push_back(t1 - t0);
        }
        print_row("add", summarize(std::move(samples)));
    }

    // --- CANCEL: pre-insert then cancel in randomized order ----------------
    {
        OrderBook book;
        std::vector<Trade> trades;
        const int total = N + warmup;
        std::vector<OrderId> ids(total);
        for (int i = 0; i < total; ++i) {
            ids[i] = static_cast<OrderId>(i);
            book.add_limit(ids[i], Side::Buy, 1000 + (i % 2000), 10, trades);
        }
        std::shuffle(ids.begin(), ids.end(), rng);  // avoid LIFO locality

        for (int i = 0; i < warmup; ++i) book.cancel(ids[i]);

        std::vector<std::uint64_t> samples;
        samples.reserve(N);
        for (int i = warmup; i < total; ++i) {
            const std::uint64_t t0 = now_ns();
            book.cancel(ids[i]);
            const std::uint64_t t1 = now_ns();
            samples.push_back(t1 - t0);
        }
        print_row("cancel", summarize(std::move(samples)));
    }

    // --- EXECUTE: each aggressor consumes exactly one resting order --------
    {
        OrderBook book;
        std::vector<Trade> trades;
        trades.reserve(4);
        const int total = N + warmup;
        // Rest one sell per price so each market buy hits exactly one maker.
        for (int i = 0; i < total; ++i)
            book.insert_order(static_cast<OrderId>(i), Side::Sell,
                              1000000 + i, 10);

        for (int i = 0; i < warmup; ++i) {
            trades.clear();
            book.add_market(static_cast<OrderId>(total + i), Side::Buy, 10, trades);
        }

        std::vector<std::uint64_t> samples;
        samples.reserve(N);
        for (int i = warmup; i < total; ++i) {
            trades.clear();
            const OrderId id = static_cast<OrderId>(total + i);
            const std::uint64_t t0 = now_ns();
            book.add_market(id, Side::Buy, 10, trades);
            const std::uint64_t t1 = now_ns();
            samples.push_back(t1 - t0);
        }
        print_row("execute", summarize(std::move(samples)));
    }

    std::printf("\nNote: latencies include ~clock-overhead ns of steady_clock "
                "call cost.\n");
    return 0;
}
