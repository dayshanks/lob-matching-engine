# feed_handler

Multithreaded TCP market-data feed handler that pipes parsed events into a limit-order-book matching engine.

## Architecture

```
[TCP feed source] ─recv()─▶ [producer: epoll + parser] ─push─▶ [SPSC<MarketEvent>] ─pop─▶ [consumer: book.apply()]
                                  pinned to CPU 2                                              pinned to CPU 4
```

- **Producer thread** — epoll-driven, edge-triggered. Reads bytes from the feed socket, frames variable-size wire messages (length-prefixed binary), parses into fixed-size `MarketEvent`s, pushes to the SPSC ring.
- **Consumer thread** — pops events, dispatches to the order book via the `BookLike` concept, records pipeline latency (recv-to-apply).
- **SPSC ring** — lock-free, cache-line-aligned head/tail to kill false sharing. Acquire/release ordering between push and pop.
- **Shutdown** — `std::stop_source` shared between threads; `std::stop_callback` writes to an `eventfd` to wake the producer's `epoll_wait` so it sees `stop_requested()` immediately.

## Requirements

- Linux (uses `epoll`, `eventfd`)
- GCC 14+ for libstdc++ `<print>` (GCC 13 works if you swap `std::println` → `std::cout << std::format(...) << '\n'`)
- `-std=c++23`

Optional for profiling:
- `perf` (Ubuntu/Debian: `linux-tools-common` + `linux-tools-$(uname -r)`)
- `git` (to clone Brendan Gregg's FlameGraph scripts)

## Build

```bash
make -j
```

Builds three binaries:
- `feed_handler` — the pipeline (the thing you benchmark)
- `feed_replay` — synthetic feed source
- `feed_handler_tsan` — same pipeline with ThreadSanitizer instrumentation

## Quick run

Two terminals:

```bash
# terminal 1
./feed_replay 9100 10000000

# terminal 2
./feed_handler 127.0.0.1 9100
```

Or use the helper:

```bash
chmod +x bench.sh flamegraph.sh    # first time only
./bench.sh 10000000
```

Expected output ends with something like:

```
events:        10000000
duration:      X.XXs
throughput:    XX.XX M msg/s
pipeline p50:  XXX ns
pipeline p99:  XXXX ns
pipeline p999: XXXXX ns
book counts:   A=... M=... X=... T=...
```

## Validate with ThreadSanitizer

```bash
./feed_replay 9100 2000000 &
./feed_handler_tsan 127.0.0.1 9100
```

Any `WARNING: ThreadSanitizer` output indicates a race. The pipeline should be silent.

## Profile with perf + flame graph

```bash
./flamegraph.sh 100000000 9100 20
# produces flame.svg — open in any browser
```

What to look for:
- Producer side dominated by `recv` / `epoll_wait` — good.
- Consumer side dominated by `book.on_*` calls — good.
- Wide `try_pop` on consumer means producer is the bottleneck.
- Wide `memmove` on producer means `RECV_BUF` is too small or messages aren't framing cleanly.

If `perf record` errors with permission denied:

```bash
sudo sysctl -w kernel.perf_event_paranoid=-1
```

(Session-scoped — fine on a dev box, don't ship it.)

## Integrate with your matching engine

The consumer dispatches into anything satisfying the `BookLike` concept (declared in `feed_protocol.h`):

```cpp
template <typename B>
concept BookLike = requires(B& b,
                            std::uint64_t oid,
                            std::uint8_t  side,
                            std::uint32_t qty,
                            std::int64_t  px,
                            std::uint16_t sym) {
    { b.on_add   (oid, side, qty, px, sym) } -> std::same_as<void>;
    { b.on_modify(oid, qty, px) }            -> std::same_as<void>;
    { b.on_cancel(oid) }                     -> std::same_as<void>;
    { b.on_trade (oid, qty) }                -> std::same_as<void>;
};
```

To swap in your real book, edit `feed_handler.cpp`:

1. Add `#include "path/to/your/order_book.h"` near the top.
2. Replace `StubBook book;` in `main()` with `YourBook book;`.
3. If your book's method names differ from `on_add` / `on_modify` / `on_cancel` / `on_trade`, rename the calls inside the `apply` lambda in `consumer_loop`.

If your method signatures already match `BookLike`, that's the entire integration. If they don't, the compiler will name the missing/mismatched method when you build — `static_assert(BookLike<YourBook>);` near the top of `main()` gives you the cleanest error.

If your matching engine lives in a sibling directory, update the Makefile's `INCLUDES` line:

```makefile
INCLUDES := -I../matching_engine/include
```

## Directory layout

```
feed_handler/
├── README.md           — this file
├── Makefile
├── .gitignore
├── spsc_ring.h         — lock-free SPSC ring (header-only)
├── feed_protocol.h     — wire format, MarketEvent, BookLike concept
├── feed_handler.cpp    — producer + consumer + main
├── feed_replay.cpp     — synthetic feed source for benchmarks
├── bench.sh            — quick smoke test
└── flamegraph.sh       — perf record + flame graph generation
```

## Tuning

Producer and consumer pin to dedicated CPUs (default 2 and 4). Override:

```bash
./feed_handler 127.0.0.1 9100 <producer_cpu> <consumer_cpu>
```

Pick cores on the same NUMA node and avoid SMT siblings of any other busy thread. On a typical 8-core laptop, `2` and `4` are fine. On a server, check `lscpu` for socket / NUMA layout first.

## Why these design choices

- **SPSC, not MPMC** — one acceptor produces, one book consumes. MPMC adds CAS overhead for no benefit.
- **Edge-triggered epoll** — fewer wakeups under load. Cost: must drain on every notification.
- **Fixed-size MarketEvent in the ring** — no allocation in the hot path. Padded to 64 bytes so adjacent ring slots never share a cache line.
- **`std::stop_source` for shutdown** — release/acquire semantics built into the language; `stop_callback` integrates with the producer's `epoll` via `eventfd`. No hand-rolled atomic flag for cooperative cancellation.
- **`BookLike` concept** — compile-time duck typing. No vtable in the dispatch hot path; any class with the right four methods plugs in.
