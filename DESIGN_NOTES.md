# Design Notes & References

This implementation was built from scratch but informed by reading prior
matching-engine designs. The most influential reference was
[jxm35/LimitOrderBook-MatchingEngine](https://github.com/jxm35/LimitOrderBook-MatchingEngine),
which I studied as a worked example of mature C++ technique. A small number
of its design choices were directly adopted; others were deliberately diverged
from in pursuit of lower per-operation latency.

## Adopted

- **First-class `Limit` class.** Each price level is its own object holding
  the FIFO head/tail and cached aggregates (`total_qty_`, `order_count_`),
  so depth queries are constant-time without walking the intrusive list.
- **CRTP publisher template.** The book is parameterised on a publisher type
  deriving from `PublisherBase<T>`, so market-data callbacks resolve at
  compile time with no virtual dispatch. A `NullPublisher` with `static`
  no-op methods is the default; the compiler eliminates it entirely from
  the matching path. A `RecordingPublisher` exists for tests.
- **Three-way invariant: order, level, and index.** Every order is reachable
  through the id index (for O(1) cancel), through the level's intrusive list
  (for FIFO match), and back to its level via cached pointers. The invariant
  is maintained across every mutating operation.

## Diverged

- **Arena-allocated raw pointers instead of `shared_ptr<OrderBookEntry>`.**
  The reference uses `shared_ptr` for every order, paying an atomic
  reference-count update on every push, pop, and cancel. This implementation
  pre-allocates a slab of `Order` slots and threads a freelist through
  `Order::next` (active and free states are disjoint, so the link slot is
  reused for free). No allocations on the hot path, no atomic refcount, no
  cache lines dirtied by refcount bumps.

  Measured against a malloc-baseline build (`-DLOB_NO_POOL`, same code path
  routed through `new` and `delete`), median of 5 runs over 2M operations on
  Apple Silicon:
  - **Throughput: +26%** (15.94M ops/sec vs 12.66M ops/sec)
  - **p50 latency: −49%** (42 ns vs 83 ns)
  - **p99 latency: −28%** (209 ns vs 291 ns)
  - **p99.9 latency: −10%** (375 ns vs 417 ns)

  The p50 result is half the median latency. That is the cache-locality win
  showing up exactly where it should: the common path benefits most from
  hot data staying resident.

- **Caller-provided order IDs with duplicate-rejection.** The reference uses
  a `static long OrderCore::ID` counter, which is not thread-safe and leaks
  state across book instances. This version takes the ID from the submitter
  and rejects duplicates via the id index. The caller now owns ID-space,
  which is the realistic model anyway: real exchanges assign exchange IDs
  separately from client IDs.

- **`std::expected<void, std::string>` for cancel.** The reference returns
  `bool` from cancel paths. Mine returns `std::expected` so the call site
  gets a typed error without exception unwinding cost on the hot path, and
  the result composes cleanly with other monadic operations.

- **Single iterator pattern in the match loop.** The reference uses an
  `erasedLimit` boolean flag inside `TryMatch` to track whether the current
  level was deleted, switching between `erase` and `++` based on the flag.
  This version re-fetches `opposite.begin()` at the top of each outer
  iteration (O(1) on `std::map`), which eliminates iterator invalidation
  as a class of bug and shortens the loop by ~15 lines.

- **Per-level batched level notifications.** A taker that sweeps `k`
  resting orders at one price emits a single `on_level_change` covering
  the full delta, not `k` separate events. The reference emits per fill.
  Per-level batching is roughly 3× fewer publisher calls in walk-heavy
  workloads, measurable in the trade-event publisher cost.

## Trade-offs not addressed

The reference includes multicast UDP publication, FIX-style order-entry TCP
server, multi-instrument exchange routing, and Python bindings. All of those
are out of scope for this project, which focuses on the matching engine
itself. A future iteration may add a thin publisher implementation behind
the CRTP hook to demonstrate the integration point without expanding scope.

L1-dcache-miss counts via `perf stat` are a planned addition once a Linux
measurement environment is set up; the current results are from Apple
Silicon, where `perf` is unavailable. The cache-locality argument is
supported indirectly by the p50 latency reduction.