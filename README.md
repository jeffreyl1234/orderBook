# Limit Order Book & Matching Engine (C++20)

[![CI](https://github.com/jeffreyl1234/orderBook/actions/workflows/ci.yml/badge.svg)](https://github.com/jeffreyl1234/orderBook/actions/workflows/ci.yml)

A single-threaded limit order book with a price-time-priority matching engine, a
NASDAQ TotalView-ITCH 5.0 replay parser, a latency benchmark harness, and a
self-contained unit-test suite. No external dependencies; CMake build.

Two matching-engine implementations behind one API:

- **v1 (`OrderBook`)** — the simplest correct design: `std::map` of price levels +
  `std::list` FIFO queues. General-purpose; drives the multi-symbol ITCH replay.
- **v2 (`FlatOrderBook`)** — a cache-friendly rewrite: flat price-indexed level
  arrays + a contiguous order arena with an intrusive free list. Same public API,
  so the benchmark drives both with an identical operation stream. **v2 is
  ~1.5–2.5× faster** on add/cancel/execute (numbers below).

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
             flat_order_book.hpp FlatOrderBook (v2, cache-friendly)
src/         order_book.cpp flat_order_book.cpp itch_parser.cpp
tests/       test_framework.hpp (tiny) + test_matching.cpp
             + test_flat.cpp + test_itch.cpp
bench/       bench_latency.cpp   (v1 vs v2 head-to-head)
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
addition. Both `add` scenarios warm the arena/allocator first so the timed region
measures steady state (a real engine pre-allocates and never grows in the hot
path), not first-touch page faults.

## v2: cache-friendly flat book, and how it wins

`FlatOrderBook` keeps v1's exact semantics (all 41 tests pass against both) but
changes the layout to attack the three costs of the map+list design:

| Cost in v1 | v2 fix |
|---|---|
| `std::map` tree descent per level access, O(log L) | flat array indexed by `(price-min)/tick`, **O(1)** |
| `malloc`/`free` per order, nodes scattered on the heap | contiguous **arena** (`std::vector<Node>`) + intrusive free list, no per-order alloc |
| pointer-chasing the level's `std::list` | FIFO threaded through the arena by 32-bit indices — compact, cache-resident |
| `map::begin()` for best price | tracked `best_bid_idx_`/`best_ask_idx_`, O(1) update, bounded rescan only when the top empties |

**The tradeoff (and why v1 stays):** a flat array must preallocate the whole
`[min,max]` price range, so v2 fits a single instrument in a bounded intraday band,
not 8,000 symbols at wildly different prices — which is exactly the multi-symbol
job v1 still does for ITCH replay. `find_order`'s returned pointer is also only
valid until the next mutating call (the arena may reallocate), a weaker guarantee
than v1's stable list iterators — the documented price of contiguity.

### Head-to-head (`./ob_bench 300000`, Apple M-series, `-O3`, 8192-tick band)

```
scenario   impl      p50      p99     p999    mean    (ns)
add        v1         83      208     2375   101.4
add        v2         42       84     1542    63.0     ~2.0x p50, ~2.5x p99
cancel     v1        708     2625    10458   868.0
cancel     v2        375     1500     4542   461.7     ~1.9x p50
execute    v1        125      417     3583   188.9
execute    v2         83      208     1542    91.6     ~1.5x p50, ~2.0x p99
```

v2 is faster on every operation at every percentile. Its `add` p50 (42 ns) sits at
the clock floor (41 ns) — the operation is now faster than `steady_clock` can
resolve, which is itself the argument for the batch-timing / `rdtsc`-on-x86 next
step.

## Tests

41 cases / 1300+ assertions via a ~100-line header-only framework (zero third-party
deps; swapping in Catch2/doctest is mechanical). Coverage includes the edge cases
that matter: partial fills (both directions), crossing at the maker's price,
multi-level sweeps, FIFO vs price priority, market orders with insufficient
liquidity, empty-level teardown, cancel of unknown ids, all three modify paths,
ITCH add/execute/cancel/delete/replace decoded from hand-built big-endian message
buffers, and the full matching suite re-run against `FlatOrderBook` plus an
arena free-list churn test.

## Roadmap (v3)

1. Batch-timing throughput numbers + `rdtsc` on x86 to get under the clock floor.
2. Hierarchical bitmap over the flat levels for O(1) best-price find (removes the
   bounded rescan) and a sparse fallback for the price tails.
3. Multi-symbol sharding: one book (or arena) per core with a lock-free ingress
   queue — the realistic path past a single-threaded serialization point.
