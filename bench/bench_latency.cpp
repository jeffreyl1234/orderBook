#include "ob/flat_order_book.hpp"
#include "ob/order_book.hpp"
#include "ob/timing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace ob;

namespace {

// Percentiles by nearest-rank on the full sorted sample set (exact, since an
// offline bench can keep every sample) so the p999 tail is trustworthy.
struct Stats {
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, min = 0, max = 0;
    double mean = 0;
    std::size_t n = 0;
};

Stats summarize(std::vector<std::uint64_t> s) {
    Stats out{};
    out.n = s.size();
    if (s.empty()) return out;
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) {
        return s[static_cast<std::size_t>(p * (s.size() - 1) + 0.5)];
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

void print_row(const char* scenario, const char* impl, const Stats& s) {
    std::printf("%-10s %-6s %10zu %8llu %8.1f %8llu %8llu %8llu %8llu\n",
                scenario, impl, s.n, (unsigned long long)s.min, s.mean,
                (unsigned long long)s.p50, (unsigned long long)s.p99,
                (unsigned long long)s.p999, (unsigned long long)s.max);
}

std::uint64_t clock_overhead_ns() {
    std::vector<std::uint64_t> d;
    d.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        const std::uint64_t a = now_ns();
        const std::uint64_t b = now_ns();
        d.push_back(b - a);
    }
    return summarize(std::move(d)).p50;
}

// Bench config. A bounded price band models a single instrument's intraday range
// and keeps the flat arrays cache-resident — the regime v2 is built for. Both
// implementations are driven with the *identical* operation stream.
constexpr Price kBase = 100000;
constexpr int   kBand = 8192;

// --- Templated scenario runners (same code drives OrderBook & FlatOrderBook) --

template <class Book>
Stats run_add(Book& book, int N, int /*warmup*/) {
    std::vector<Trade> tr;
    tr.reserve(8);
    // Steady-state warm-up: insert then cancel N orders so the arena is fully
    // grown and its pages are resident, and the allocator/free-list is primed.
    // This mirrors a real engine that pre-allocates at startup and never grows
    // the arena in the hot path — otherwise page faults during the timed region
    // dominate the tail and measure the OS, not the book.
    for (int i = 0; i < N; ++i) {
        book.add_limit(static_cast<OrderId>(i), Side::Buy,
                       kBase + (i % kBand), 10, tr);
        tr.clear();
    }
    for (int i = 0; i < N; ++i) book.cancel(static_cast<OrderId>(i));

    std::vector<std::uint64_t> samples;
    samples.reserve(N);
    for (int i = 0; i < N; ++i) {
        const OrderId id = static_cast<OrderId>(N + i);
        const Price px = kBase + (i % kBand);
        const std::uint64_t t0 = now_ns();
        book.add_limit(id, Side::Buy, px, 10, tr);
        const std::uint64_t t1 = now_ns();
        tr.clear();
        samples.push_back(t1 - t0);
    }
    return summarize(std::move(samples));
}

template <class Book>
Stats run_cancel(Book& book, int N, int warmup, std::mt19937_64& rng) {
    const int total = N + warmup;
    std::vector<Trade> tr;
    std::vector<OrderId> ids(total);
    for (int i = 0; i < total; ++i) {
        ids[i] = static_cast<OrderId>(i);
        book.add_limit(ids[i], Side::Buy, kBase + (i % kBand), 10, tr);
    }
    std::shuffle(ids.begin(), ids.end(), rng);  // defeat LIFO locality

    for (int i = 0; i < warmup; ++i) book.cancel(ids[i]);

    std::vector<std::uint64_t> samples;
    samples.reserve(N);
    for (int i = warmup; i < total; ++i) {
        const std::uint64_t t0 = now_ns();
        book.cancel(ids[i]);
        const std::uint64_t t1 = now_ns();
        samples.push_back(t1 - t0);
    }
    return summarize(std::move(samples));
}

template <class Book>
Stats run_execute(Book& book, int N, int warmup) {
    const int total = N + warmup;
    std::vector<Trade> tr;
    tr.reserve(8);
    // One resting sell per (id) across the band; each market buy of 10 consumes
    // exactly one maker, so we time the match+remove path.
    for (int i = 0; i < total; ++i)
        book.insert_order(static_cast<OrderId>(i), Side::Sell,
                          kBase + (i % kBand), 10);

    for (int i = 0; i < warmup; ++i) {
        tr.clear();
        book.add_market(static_cast<OrderId>(total + i), Side::Buy, 10, tr);
    }
    std::vector<std::uint64_t> samples;
    samples.reserve(N);
    for (int i = warmup; i < total; ++i) {
        tr.clear();
        const OrderId id = static_cast<OrderId>(total + i);
        const std::uint64_t t0 = now_ns();
        book.add_market(id, Side::Buy, 10, tr);
        const std::uint64_t t1 = now_ns();
        samples.push_back(t1 - t0);
    }
    return summarize(std::move(samples));
}

} // namespace

int main(int argc, char** argv) {
    int N = 200000;
    if (argc > 1) N = std::atoi(argv[1]);
    const int warmup = N / 10;
    const std::size_t cap = static_cast<std::size_t>(N + warmup + 16);

    std::printf("Order book latency benchmark — v1 (std::map+list) vs "
                "v2 (flat array+arena)\n");
    std::printf("single-threaded  ops/scenario: %d  warm-up: %d  "
                "price band: %d ticks\n", N, warmup, kBand);
    std::printf("clock: steady_clock/now_ns  overhead p50: %llu ns\n\n",
                (unsigned long long)clock_overhead_ns());

    std::printf("%-10s %-6s %10s %8s %8s %8s %8s %8s %8s\n", "scenario", "impl",
                "count", "min", "mean", "p50", "p99", "p999", "max");
    std::printf("%-10s %-6s %10s %8s %8s %8s %8s %8s %8s  (ns)\n", "", "", "", "",
                "", "", "", "", "");

    std::mt19937_64 rng(12345);

    {   OrderBook v1;      print_row("add", "v1", run_add(v1, N, warmup)); }
    {   FlatOrderBook v2(kBase, kBase + kBand, 1, cap);
        print_row("add", "v2", run_add(v2, N, warmup)); }

    {   OrderBook v1;      print_row("cancel", "v1", run_cancel(v1, N, warmup, rng)); }
    {   FlatOrderBook v2(kBase, kBase + kBand, 1, cap);
        print_row("cancel", "v2", run_cancel(v2, N, warmup, rng)); }

    {   OrderBook v1;      print_row("execute", "v1", run_execute(v1, N, warmup)); }
    {   FlatOrderBook v2(kBase, kBase + kBand, 1, cap);
        print_row("execute", "v2", run_execute(v2, N, warmup)); }

    std::printf("\nNote: latencies include ~clock-overhead ns of steady_clock "
                "call cost; compare v1 vs v2 relatively, not absolutely.\n");
    return 0;
}
