# ADR 0002: Counterfactual matching versus factual feed reconstruction

- Status: Accepted
- Date: 2026-08-25
- Normative protocol: [Nasdaq TotalView-ITCH 5.0, June 11 2026](https://assets.ctfassets.net/mx0rke14e5yt/5Uz6MGJxbo4wRPou8KveFs/4d76437c8e57694acee9d767587a8dfa/6-11-26_TVITCH_5.0_1.pdf)

## Context

Round 1 answers a counterfactual question: given an inbound command and the current local book,
what executions would a deterministic price-time-priority matching engine create? TotalView-ITCH is
an outbound fact stream. Its order execution, cancellation, deletion, replacement, trade, cross, and
break messages report what Nasdaq already did. Feeding those facts to the Round 1 matcher would
invent aggressors, prices, and executions that are absent from the historical feed.

## Decision

The Round 1 `lob::OrderBook` remains a separate library and is not called by Round 2. Round 2 owns a
new `lob::itch` decoder and `lob::replay::FactualState`.

Only `A` and `F` create displayed orders. `E` and `C` reduce the referenced displayed order; `C`
records its separate execution price without moving the displayed order. `X` reduces displayed
shares, `D` removes all remaining shares, and `U` removes the old reference and appends a new
reference with inherited side, symbol, and attribution but new size, price, and FIFO priority.
Messages `P`, `Q`, and `B` update the trade ledger and statistics without changing displayed depth.
All other typed messages update session or reference state.

The factual book uses its own ordered price maps, FIFO lists, aggregate caches, and order-reference
index because those neutral data structures fit both domains. No matching rules, crossing tests,
time-in-force behavior, or counterfactual trade generation are reused.

## Consequences

Historical replay is deterministic and auditable: each book mutation is traceable to exactly one
outbound feed fact. It cannot answer queue-fill or hypothetical-order questions; those remain Round
1 responsibilities. The separation also permits independent invariants, a slow factual oracle, and
future adapter changes without coupling the exchange feed to matching policy.

## Rejected alternatives

- Translating ITCH executions into market orders was rejected because it would rematch liquidity and
  create false taker identities and prices.
- Treating `C`'s execution price as a displayed-price replacement was rejected because the
  specification says the execution can differ from the original display price.
- Mutating depth for `P`, `Q`, or `B` was rejected because those messages concern non-displayed
  trades, cross prints, and broken executions rather than current displayed orders.
