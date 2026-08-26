# LOBForge

LOBForge is a deterministic C++20 market-microstructure foundation with three deliberately separate
domains. Round 1 is a single-symbol counterfactual matching engine: a caller supplies hypothetical
commands and receives the executions that local price-time rules would create. Round 2 is a
field-complete Nasdaq TotalView-ITCH 5.0 decoder and factual historical-feed reconstructor: it
replays exchange facts without rematching them.

Both cores avoid wall-clock input, logging, networking, and hidden concurrency. The Round 2 parser
uses explicit bounds-checked big-endian reads, integer fixed-point Price(4), typed representations
for all 23 application messages in the June 11, 2026 specification, and deterministic state
serialization. File I/O and text/JSON presentation live only in `lobforge_replay`.

Round 3 adds a deterministic Python 3.11 research layer without moving factual authority out of
C++. The replay CLI emits versioned `book_event/v1` NDJSON; the installable
`lobforge-research` package validates the stream in bounded batches, writes fixed-schema Parquet and
canonical logical digests, computes microstructure features/labels, and runs leakage-controlled
interpretable evaluations. NDJSON is an auditable research boundary, not a production feed bus.

Round 4 adds a third, deliberately separate C++20 domain: a deterministic shadow-order simulator.
It observes the Round 2 factual book read-only, schedules market/compute/order/cancel/replace
latencies, tracks counterfactual queue position, applies evidence-bounded fills, runs three
interpretable market-making strategies and maintains exact nanodollar inventory/PnL accounting.
It never inserts shadow liquidity into the factual book and has no live order-entry capability.

## Architecture and API

```text
Round 1: Command -> matching policy -> counterfactual events

Round 2: u16-BE framed bytes -> safe decoder -> typed ITCH facts
                                      -> session/reference state
                                      -> factual displayed book + trade ledger
                                      -> canonical state + FNV-1a digest

Round 3: Round 2 facts -> book_event/v1 NDJSON -> strict Arrow validation
                                           -> Parquet + semantic manifest
                                           -> features/labels -> chronological evaluation

Round 4: Round 2 facts (read only) -> deterministic latency scheduler
                                  -> separate shadow lifecycle + queue evidence
                                  -> risk-gated strategy intent -> exact ledger
                                  -> versioned audits -> independent Python oracle/report
```

Round 1 and Round 2 do not call each other. The reason and exact semantic boundary are recorded in
[ADR 0002](docs/adr/0002-counterfactual-matching-versus-factual-reconstruction.md); the offline
record envelope and error policy are in [ADR 0003](docs/adr/0003-offline-framing-and-error-policy.md).

`lob::OrderBook::process(const Command&)` dispatches `NewOrder`, `CancelOrder`, `ReduceOrder`, and
`ReplaceOrder`. Validation happens before book mutation. Accepted aggressive orders walk the best
opposite-side price first and the oldest order at that price first. The method returns events in the
exact transition order. Separate monotonic counters provide event sequences and resting-priority
sequences; caller timestamps are stored as metadata and never decide priority.

The production book contains:

- a descending `std::map` for bids and ascending `std::map` for asks;
- one `std::list` FIFO per price level, with a checked cached aggregate quantity;
- an `std::unordered_map<OrderId, IndexEntry>` whose stable list iterator permits direct removal.

The public read-only inspection surface provides best bid/ask, aggregate quantity at a level, top-N
or full aggregate depth, active order and price-level counts, order remaining quantity/priority/queue
position, ordered active-order views, a canonical textual snapshot, and an invariant checker. It
never exposes mutable containers. See [the public headers](include/lob/order_book.hpp) and the
[semantics ADR](docs/adr/0001-matching-engine-semantics.md).

## Matching and command semantics

- Better prices precede worse prices; resting priority at one price is FIFO by strictly increasing
  priority sequence.
- A crossing order always trades at the maker's resting price. Buys cross asks at or below their
  limit; sells cross bids at or above their limit. Partial maker and taker fills are supported.
- A GTC limit matches immediately and rests a remainder. IOC matches and emits `Expired` for a
  remainder. FOK first scans eligible liquidity without mutation; insufficient liquidity produces
  `Rejected(CannotFullyFill)`. PostOnly rejects with `WouldCross` if it would immediately trade,
  otherwise it rests. Market orders support IOC and FOK only and never rest.
- An active duplicate ID is rejected. A zero quantity is rejected. A limit requires a positive
  price; a market must omit price. Market GTC/PostOnly combinations are rejected. All rejection
  classifications are the `RejectReason` enum, not strings.
- Cancel removes the entire remaining quantity. Reduce accepts a positive `reduce_by` strictly less
  than the remaining quantity; zero, equal, or larger reductions are rejected and never act as a
  cancel. Unknown IDs for cancel, reduce, and replace are rejected.
- A same-price replacement with a smaller quantity changes the cached aggregate in place and keeps
  priority. Same price and same quantity emits an explicit priority-retaining `Replaced` no-op and
  leaves stored timestamp and priority unchanged. A price change or quantity increase loses
  priority: after complete validation it atomically removes the old order and acts as a new GTC
  limit with the same ID and a new priority sequence, potentially trading immediately. Failed
  validation preserves the original order.
- A level aggregate that would exceed `UINT64_MAX` is rejected before mutation. FOK availability is
  accumulated with saturation at the requested quantity, so its scan cannot overflow.

Every command produces observable state-transition events: `Accepted`, `Rejected`, `Trade`,
`Rested`, `Cancelled`, `Reduced`, `Replaced`, and `Expired`. A trade identifies maker, taker,
resting price, quantity, aggressive side, and event sequence.

After each command the debug build asserts the invariant checker. The test suite also calls it after
every deterministic and randomized command. It verifies nonzero orders, bid/ask separation, index
and container bijection, iterator metadata, cached aggregates, nonempty levels, strict FIFO
sequences, indexed/reachable counts, and checked aggregate arithmetic.

## Complexity

Let `P` be active price levels, `N` active orders, `L` price levels crossed, `F` makers filled or
partially filled, and `Q` orders at one level. `unordered_map` bounds below are expected-case; its
adversarial worst case is `O(N)`.

| Operation | Actual complexity |
|---|---|
| Insert a non-crossing resting order | `O(log P)` map lookup/insertion + expected `O(1)` index insertion and `O(1)` FIFO append |
| Best bid or ask | `O(1)` via `map::begin()` |
| Cancel after ID lookup | `O(1)` list erase and amortized `O(1)` map iterator erase if the level empties |
| Reduce after ID lookup | `O(1)` |
| Match an aggressive order | `O(L + F)` ordered traversal plus expected `O(F)` index erases |
| Aggregated top-N/full snapshot | `O(min(P, topN))` / `O(P)` |
| Canonical per-order snapshot or all order views | `O(P + N)` |
| Queue position for one known order | expected `O(1)` ID lookup + `O(Q)` FIFO walk |

## Build and validate

CMake 3.20+, a C++20 compiler, and Ninja are recommended. On this Windows host the commands used
the CMake, Ninja, and GCC 13.1 executables bundled with CLion by placing their directories on
`PATH`; after that setup the project commands are conventional:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Generate the license-safe full-session fixture and replay it:

```sh
./build/itch_replay_tests --write-full-session build/synthetic_full_session.itch
./build/lobforge_replay --input build/synthetic_full_session.itch \
  --strict --symbol AAPL --top 10 --format text
```

Strict mode is the default. `--permissive` skips only bad records whose complete framing is known;
an incomplete envelope or payload at EOF remains terminal. See the complete
[CLI reference](docs/replay_cli.md), [23-type coverage matrix](docs/itch50_coverage.md), and
[Round 2 validation report](docs/round2_validation_report.md).

ASan, UBSan, and leak detection are supported with GCC/Clang runtimes. A Linux/WSL example is:

```sh
cmake -S . -B build-sanitize -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_SANITIZERS=ON \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build-sanitize
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize --output-on-failure
```

The current WSL image has no Ninja, so the executed local sanitizer validation omitted `-G Ninja`
and used CMake's Unix Makefiles. This does not change compiler or sanitizer coverage.

Formatting is pinned by `.clang-format` and can be checked without rewriting files:

```sh
cmake --build build --target format-check
```

The dependency-free test executable reports 27 named cases. Its differential case sends 5,000
commands for each of 20 fixed seeds (100,000 total) through both the production engine and an
intentionally slow scan-based reference, comparing full events, best prices, aggregate depth,
active IDs/quantities/priority metadata, and invariants after every command.

## Round 3 quick start

Create the locked Python environment and freeze the preregistered protocol:

```sh
cd python
uv sync --frozen --all-groups
uv run lobforge-research freeze-protocol --protocol ../configs/round3_protocol.toml
```

Build a dataset through the C++ subprocess boundary:

```sh
uv run lobforge-research build-dataset \
  --replay-cli ../build/lobforge_replay \
  --input session.itch --session-date 2026-08-24 \
  --output ../artifacts/session_2026-08-24 --depth 10 --batch-rows 65536
```

Run the license-safe controls, quality gates and full local performance workloads:

```sh
uv run lobforge-research synthetic-report --output ../artifacts/synthetic_round3
uv run ruff check src tests
uv run ruff format --check src tests
uv run mypy src
uv run pytest --cov=lobforge_research --cov-branch
uv run lobforge-research benchmark-features --rows 1000000 --runs 3
uv run lobforge-research benchmark-pipeline --rows 1000000 --runs 3 --batch-rows 65536
```

The schema contract is [book_event_v1](docs/book_event_v1.md), feature definitions are in the
[dictionary](docs/feature_dictionary.md), and the frozen experimental design and leakage evidence
are in the [methodology](docs/round3_methodology.md) and
[leakage audit](docs/round3_leakage_audit.md). The
[Round 3 validation report](docs/round3_validation_report.md) keeps engineering gates separate from
real-data provenance/coverage and the primary signal status.
The executed million-row measurements are in the
[Round 3 benchmark report](docs/round3_benchmark.md), and the exact dependency policy is covered by
the [dependency/license audit](docs/round3_dependency_license_audit.md).

## Round 4 quick start

Run the deterministic end-to-end fixture and analyze its immutable audit artifacts:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lobforge_mm_sim \
  --synthetic-fixture primary \
  --config configs/round4_protocol.toml \
  --output-dir artifacts/round4_primary
cd python
uv run lobforge-research round4-analyze \
  --input ../artifacts/round4_primary \
  --output ../artifacts/round4_analysis
uv run lobforge-research round4-synthetic-report \
  --output ../artifacts/round4_controls
```

For a user-supplied lawful ITCH file, replace `--synthetic-fixture primary` with `--input
<file>`. The architecture/methodology is in [the Round 4 design](docs/round4_architecture.md), queue
semantics in [ADR 0004](docs/adr/0004-shadow-queue-and-fill-semantics.md), schemas in
[the audit contract](docs/shadow_audit_v1.md), CLI behavior in [the CLI reference](docs/mm_sim_cli.md),
the [counterfactual methodology](docs/round4_methodology.md), every formula in
[the metric dictionary](docs/round4_metrics.md), and the [benchmark evidence](docs/round4_benchmark.md).
The full evidence and honest
F1-F3/P1 status are in [the validation report](docs/round4_validation_report.md).

## Benchmark

Build Release, generate all streams before timing, then run:

```sh
./build/order_book_benchmark
```

The default uses `std::mt19937_64` seed `12648430`, performs 100,000 warm-up commands on a separate
book, and runs at least 1,000,000 measured commands for each of `add_cancel`, `crossing`, `mixed`,
and `deep_book`. Throughput uses one
`steady_clock` interval around the sequential command loop. A second fresh replay samples every
command with `steady_clock`, sorts nanoseconds, and reports nearest-rank p50/p95/p99. RNG,
generation, sorting, invariant checking, checksums, and output are outside timed regions. The
program checks that throughput and latency replays finish at identical canonical snapshots and
prints a deterministic FNV-1a-derived checksum. `--smoke` uses 10,000 commands for CI without a
performance threshold; `--commands N` selects another diagnostic count.

Measured results and complete environment disclosure are in
[the Round 1 engineering report](docs/round1_report.md). Shared-runner CI only runs the smoke mode.

Round 2's in-memory benchmark is `itch_replay_benchmark`. It measures mixed 23-type decoding,
decoder-plus-book apply, a deep multi-symbol book, and permissive error handling. Methodology and
measured results are in [the Round 2 benchmark report](docs/round2_benchmark.md).

## Limitations / not production trading

Round 2 is an offline application-message replayer, not an exchange client or trading system. It
does not implement SoupBinTCP, MoldUDP64, GLIMPSE, packet capture, gap recovery, compression, live
sockets, order entry, Python bindings, strategies, risk controls, persistence, a GUI, or production
operations. Synthetic fixtures demonstrate deterministic engineering behavior; no official or
user-supplied market-data file was available in this workspace, so real-data evidence remains
`BLOCKED: DATASET_NOT_PROVIDED`. No performance or correctness result is evidence of profitability.

Round 4 now implements offline counterfactual quoting, queue/fill evidence, deterministic latency,
fees/rebates, inventory, PnL/risk and post-fill markouts. It still excludes market impact,
counterfactual behavior by other participants, hidden-liquidity inference, live protocols, broker
APIs, real/paper trading, production operations, deep learning, dashboards and databases. Its
synthetic controls prove only that the implementation responds to planted mechanics. With no lawful
real dataset in the workspace, provenance, real counterfactual replay, out-of-sample robustness and
profitability are all `BLOCKED: DATASET_NOT_PROVIDED`.

## Worked example

With one-cent ticks, submit these GTC limit orders in order:

1. Sell ID 1: 100 shares at `10002` ($100.02) → `Accepted`, then `Rested(100)`.
2. Sell ID 2: 50 shares at `10002` ($100.02) → `Accepted`, then `Rested(50)`.
3. Sell ID 3: 100 shares at `10003` ($100.03) → `Accepted`, then `Rested(100)`.
4. Buy ID 4: 180 shares at `10003` ($100.03) → `Accepted`, then these trades:
   `maker=1, taker=4, price=10002, qty=100`; `maker=2, taker=4, price=10002, qty=50`;
   `maker=3, taker=4, price=10003, qty=30`.

The execution demonstrates lower ask price first, FIFO between IDs 1 and 2, maker-price execution,
a multi-level sweep, and a partial maker fill. ID 4 is fully filled. The final book has no bids and
one ask: ID 3 with 70 shares remaining at `10003` ($100.03).
