# ADR 0004: Shadow queue and counterfactual fill semantics

- Status: Accepted
- Date: 2026-08-26
- Frozen protocol: [`round4_protocol.toml`](../../configs/round4_protocol.toml)

## Context

The factual ITCH reconstructor reports what the venue did to displayed orders. A hypothetical
market-maker order did not exist in that history, so its activation, queue position and fill are
counterfactual claims. Inserting it into the factual book would corrupt historical state; sending
ITCH executions through the Round 1 matcher would invent aggressors and liquidity.

## Decision

`lob::replay::FactualState` remains the sole factual authority. `lob::mm::ShadowSimulator` receives
post-apply factual mutations through read-only inspection and maintains separate shadow orders,
queue snapshots, fills and accounting. Strategies receive immutable market snapshots and return
quote intentions. Neither component can mutate factual state.

The primary model is `mbo_fifo_conservative`. Activation snapshots every extant factual order at
the shadow price as queue-ahead. Later additions are behind. `E`, `C` and `X` reduce a known
ahead reference; `D` and `U` remove it. Reaching zero does not fill the shadow order by itself. A
later same-price `E`/`C` on a behind reference is evidence only when queue-ahead is zero. A displayed
execution strictly worse than the shadow limit is trade-through evidence. `C` is charged at the
shadow limit even when its reported execution price improves, while its queue deduction remains at
the original displayed price. `P`, `Q` and `B` cannot create displayed shadow fills.

`trade_through_only` is the pessimistic lower sensitivity. `front_of_queue` treats any same-price
displayed execution as fill evidence and is an optimistic upper sensitivity; it is never primary
market evidence. Immediately marketable shadow orders are rejected and quotes may use only observed
factual levels.

For equal effective timestamps, all factual messages are applied before local observation,
decision, activation, cancel confirmation and replace confirmation events. Pending cancel/replace
orders remain exposed. A replace takes a new priority sequence and a fresh factual queue snapshot.

## Consequences and limitations

Every fill has explicit factual evidence and stable reason fields. This is conservative and
auditable, but it is not a venue queue-position oracle: hidden liquidity, self-impact, message loss,
latency jitter, participant-specific priority and execution feasibility are not inferred. The
model validates counterfactual mechanics on synthetic controls; it cannot establish real fills or
profitability without separately gated licensed data.
