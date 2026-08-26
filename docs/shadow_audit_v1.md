# Round 4 shadow audit schemas

All files are UTF-8 NDJSON with LF endings, fixed JSON key order and integer monetary values. Null
means unavailable; zero is never substituted for a missing value. Schema identifiers and versions
must match exactly. Unknown major versions are rejected by the Python analysis layer.

## `lobforge.shadow_order`, version 1

Fields, in order: `schema`, `version`, `timestamp_ns`, `local_sequence`, `order_id`, `symbol`,
`side`, `price4`, `quantity`, `remaining_quantity`, `from_state`, `to_state`, `reason`, and
`queue_ahead_quantity`. Each legal transition and each rejected illegal request emits a record.
States include submitted, pending acknowledgement, active, pending cancel, pending replace,
partially/fully filled and the rejected/cancelled/session/risk terminal states.

## `lobforge.shadow_fill`, version 1

Fields: `schema`, `version`, `timestamp_ns`, `factual_sequence`, `local_sequence`, `order_id`,
`symbol`, `side`, `quantity`, `accounting_price4`, `factual_display_price4`,
`factual_execution_price4`, `anchor_mid2`, `match_number`, `reason`, `fee_nanos`, and
`rebate_nanos`. `accounting_price4` is the conservative shadow limit. `anchor_mid2` is causal state
known immediately before the factual mutation; future markout prices are absent.

## `lobforge.inventory_event`, version 1

Fields: `schema`, `version`, `timestamp_ns`, `local_sequence`, `order_id`, signed `inventory`,
`trade_cash_nanos`, cumulative `fees_nanos`, cumulative `rebates_nanos`,
`realized_gross_pnl_nanos`, and nullable midpoint/net/conservative equities. Equities are null when
the causal book needed to value them is unavailable.

## `lobforge.mm_summary`, version 1

The summary records the complete protocol SHA-256, event/order/fill/risk counts, exact integer
accounting, inventory statistics, quote-time statistics, right-continuous markouts and the semantic
digest. Ratios are decimal presentation fields derived from integer numerator/denominator pairs;
zero denominators produce JSON null. The digest uses canonical typed logical simulator state, not
filesystem metadata, temporary paths, system time or raw output bytes.

Compatibility is additive within version 1: readers must reject a changed schema identifier or
major version and may accept documented appended fields. Changing field meaning, units,
nullability or ordering requires version 2.
