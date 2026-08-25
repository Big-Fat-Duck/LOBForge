# ADR 0001: Deterministic matching-engine semantics

- Status: Accepted
- Date: 2026-08-25
- Scope: Round 1, one symbol, one in-process thread

## Context

Round 1 needs an inspectable correctness foundation that produces identical logical output from an
identical initial state and command stream. It must support price-time matching, four time-in-force
policies, indexed order management, atomic priority-losing replacement, overflow-safe aggregation,
and differential testing without pulling a framework into the core.

## Decisions

The caller owns scheduling. `OrderBook::process` accepts one value-type command and synchronously
returns an ordered vector of value-type events. The book never observes wall time. Caller timestamps
are inert metadata; monotonic engine counters determine event order and resting FIFO priority.

Prices are positive signed 64-bit ticks; quantities, IDs, timestamps, event sequences, and priority
sequences are unsigned 64-bit integers. Matching never uses floating point. A crossing execution is
priced at the resting maker's price. Bids are a descending ordered map, asks an ascending ordered
map, each level owns a FIFO list, and a hash index stores side, price, and a stable list iterator.
Each level caches aggregate quantity and every addition is checked for overflow.

GTC limit remainders rest. IOC and market remainders emit `Expired`; market supports IOC and FOK
only. FOK performs a non-mutating, overflow-safe eligible-liquidity scan and rejects before book
mutation if short. PostOnly performs a best-price crossing check and rejects before mutation if it
would trade. An active duplicate ID is rejected before any book change.

`ReduceOrder::reduce_by` must be positive and strictly smaller than open quantity. It keeps FIFO
priority. Cancel removes all open quantity. Same-price replacement to a smaller quantity updates in
place. Same-price/same-quantity replacement is an explicit `Replaced` no-op; priority and stored
timestamp remain unchanged. Any price change or quantity increase is validated in full, including
the projected post-match resting aggregate, before removal. It then removes the old order and acts
as an aggressive GTC limit with the same ID and a fresh priority sequence. This allows immediate
execution while preserving the original on validation failure.

Rejected commands still receive monotonic event sequence numbers, but canonical book state contains
only resting state. The canonical representation orders prices and FIFO orders explicitly and
includes IDs, remaining quantities, priority sequences, and caller timestamps.

## Invalid input policy

Zero quantity, missing/nonpositive limit price, a priced market order, market GTC/PostOnly,
duplicate active ID, unknown managed ID, zero/equal/excessive reduction, invalid replacement, FOK
shortfall, PostOnly crossing, aggregate overflow, and sequence exhaustion each map to a typed
`RejectReason`. There are no internal string classifications. A rejection preserves resting state.

## Invariants

Debug command processing asserts a public read-only checker. Tests invoke it after every randomized
step. The checker proves that active quantities are nonzero, every order is reachable once and
indexed once, index metadata and iterators point back to the same object, cached aggregates equal
checked sums, empty levels are absent, FIFO sequences increase strictly, reachable/index counts
agree, and best bid is strictly below best ask.

## Consequences

Best-price lookup is constant time. A resting insert costs `O(log P)` plus expected constant-time
index work. Indexed reduce and list removal are constant time after an expected constant-time hash
lookup (with standard hash-table worst-case caveats). Matching is linear in crossed levels and
makers. Aggregate snapshots cost `O(P)` and canonical per-order snapshots cost `O(P + N)`. A single
order's queue position requires an `O(Q)` FIFO walk.

The engine is intentionally single-threaded and not exception-transactional under allocation
failure. It provides logical atomicity for all validation failures. Adapters may serialize event
values later, but mutable internals remain private.

## Rejected alternatives

- Floating-point prices were rejected because equality, ordering, and serialization are less
  deterministic than integer ticks.
- A flat vector-only production book was rejected because cancellation and best-price access would
  require scans. It remains appropriate for the independent test oracle.
- Heap-only price levels were rejected because cancellation and deterministic full-depth inspection
  become awkward.
- A global mutex or internal worker thread was rejected because Round 1 is caller-serialized and a
  lock would add policy and latency without correctness value.
- Caller timestamp priority was rejected because equal or non-monotonic external timestamps make
  FIFO ambiguous.
- Treating reduce-to-zero as cancel was rejected so command intent and resulting events stay
  explicit.
- In-place price changes and size increases were rejected because they violate exchange-style queue
  fairness.
- Third-party test and benchmark frameworks were rejected for this foundation because a small
  deterministic harness covers the required surface without network-fetched or unpinned code.
