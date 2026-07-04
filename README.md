# Limit Order Book & Matching Engine (C++20)

[![CI](https://github.com/jeffreyl1234/orderBook/actions/workflows/ci.yml/badge.svg)](https://github.com/jeffreyl1234/orderBook/actions/workflows/ci.yml)

A single-threaded limit order book with a price-time-priority matching engine, a
NASDAQ TotalView-ITCH 5.0 replay parser, a latency benchmark harness, and a
self-contained unit-test suite. No external dependencies; CMake build.

This is **v1: the simplest correct design** (`std::map` of price levels + FIFO
queues). It is deliberately structured so the cache-friendly flat-array layout
(v2) is a drop-in replacement behind the same API, letting us A/B the benchmarks.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # or: ./build/ob_tests
./build/ob_bench 200000                       # latency benchmark
./build/itch_replay <file.itch>               # replay ITCH, or '-' for stdin
```

To replay real data (gzip'd samples from `emi.nasdaq.com`) without linking a
decompression lib, stream it through the system `gunzip`:

```sh
gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay -
```

## Architecture

```
include/ob/  types.hpp      OrderId/Price/Quantity/Side/Trade
             order.hpp      resting Order
             order_book.hpp OrderBook: matching + passive primitives
             itch_parser.hpp ItchReplayer
             timing.hpp     portable cycle/time source
src/         order_book.cpp itch_parser.cpp
tests/       test_framework.hpp (tiny) + test_matching.cpp + test_itch.cpp
bench/       bench_latency.cpp
apps/        itch_replay.cpp
```

### Data structure (v1)

```
bids: std::map<Price, Level, std::greater>   // begin() == best (highest) bid
asks: std::map<Price, Level, std::less>      // begin() == best (lowest)  ask
Level = { std::list<Order> orders; Quantity total; }   // FIFO, front = oldest
index: std::unordered_map<OrderId, {Side, Price, list::iterator}>
```

| Operation        | Cost        | Why                                              |
|------------------|-------------|--------------------------------------------------|
| best bid/ask     | O(1)        | `map::begin()`                                   |
| add (rest)       | O(log L)    | find/create the price level                      |
| match one level  | O(fills)    | FIFO pop from the front of the list              |
| cancel by id     | O(1)*       | index gives the stable list iterator directly    |
| depth at price   | O(1)        | `Level::total` maintained incrementally          |

`L` = number of distinct price levels. *cancel is O(1) list splice plus an O(log L)
map lookup only to tear down a level that just emptied.

## Design decisions (and the tradeoffs I can defend)

- **Integer tick prices, never `double`.** Levels are keyed by exact equality;
  `0.1 + 0.2 != 0.3` in IEEE-754 would fracture a level into phantom prices.
  ITCH also encodes price as fixed-point `int`, so integers are the native form.

- **`std::map` + `std::list`, not a hand-rolled structure — yet.** The map keeps
  levels sorted for O(1) best-price and clean iteration; the list gives *stable
  iterators*, so stashing an order's iterator in the index makes cancel O(1)
  instead of a linear level scan. The cost is a node allocation per order and
  pointer-chasing on the hot path — exactly what v2's flat arrays will attack.

- **Matching and passive mutation are separate primitives.** `add_limit` =
  *match* then passively *insert* the remainder. ITCH replay drives only the
  passive primitives (`insert_order`/`reduce_order`/`remove_order`) because the
  exchange already matched — running our matcher over a live feed would be wrong
  (feeds legitimately show transient locked/crossed books). One insert primitive,
  two entry points.

- **Execution price is the maker's price, not the taker's limit.** A buy limit at
  105 hitting a resting ask at 100 trades at 100. This is the definition of
  price-time priority and the source of "price improvement."

- **Modify follows NASDAQ rules.** Pure size *decrease* at the same price keeps
  time priority (mutate in place). A price change or size *increase* is a
  cancel + re-add: it loses time priority and may cross. Both are tested.

- **Market orders never rest.** Any unfilled remainder after sweeping the book is
  discarded rather than parked.

- **Single-threaded, by design.** A matching engine is a serialization point; the
  realistic scaling story is one book (or shard) per core with a lock-free ingress
  queue, not locks around a shared book. That's a later chapter, not v1.

## Benchmark methodology

`ob_bench` times each operation individually and reports p50/p99/p999 (nearest-rank
on the full sorted sample set — exact, since we keep every sample offline). It
warms up 10% of iterations (discarded), reuses the trade output vector to keep the
hot path allocation-free, and randomizes cancel order to defeat LIFO cache locality.

**Honest caveat about the clock on this machine.** `rdtsc` is x86-only and absent
on arm64. We use `std::chrono::steady_clock` (`mach_absolute_time`), but on Apple
Silicon that is backed by a ~24 MHz timer → **~41 ns granularity**, which the
harness measures and prints. For sub-100 ns operations the per-op `p50` is
therefore quantized (you'll see a `min` of 0); the `mean` column is the more
trustworthy signal at that scale. The x86 `rdtsc` path is wired up in `timing.hpp`
for when this runs on an Intel box; batch-timing throughput is the natural next
addition. Representative run (`./ob_bench 100000`, Apple M-series, `-O3`):

```
scenario           count   min   mean   p50   p99  p999    max   (ns)
add               100000     0  199.1    83   750 13625   ...   pure resting insert
cancel            100000    41  751.8   584  2584 15875   ...   index lookup + level teardown
execute           100000    83  322.3   208   959 14083   ...   one fill per aggressor
clock overhead p50: 41 ns
```

## Tests

28 cases / 100+ assertions via a ~100-line header-only framework (zero third-party
deps; swapping in Catch2/doctest is mechanical). Coverage includes the edge cases
that matter: partial fills (both directions), crossing at the maker's price,
multi-level sweeps, FIFO vs price priority, market orders with insufficient
liquidity, empty-level teardown, cancel of unknown ids, all three modify paths,
and ITCH add/execute/cancel/delete/replace decoded from hand-built big-endian
message buffers.

## Roadmap (v2)

1. Flat-array price levels indexed by `(price - base) / tick` for O(1) level
   access and contiguous iteration; intrusive freelist for orders (no per-order
   `malloc`).
2. Benchmark v1 vs v2 head-to-head with the same harness.
3. Batch-timing throughput numbers to complement the quantized per-op latencies.
