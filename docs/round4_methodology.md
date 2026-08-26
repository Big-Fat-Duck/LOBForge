# Round 4 counterfactual methodology

## Question and evidence boundary

Round 4 asks whether a historical displayed-order stream can drive a deterministic, conservative
shadow-order experiment without contaminating exchange facts. It does not ask whether the simulated
orders were executable in a real market. Synthetic outcomes validate mechanics only; F1-F3/P1 remain
separate and require licensed, provenance-complete real sessions.

## Causal timeline

For factual event time `t`, the strategy receives an immutable snapshot at
`t + market_data_latency`. Its decision completes after `strategy_compute_latency`; a new order is
eligible only after `order_entry_latency`. Cancelled or replaced orders remain exposed until their
respective confirmation latency. The total ordering key is
`(effective_timestamp, factual_sequence, category, local_sequence)`. All factual events at a tied
timestamp precede local events.

The primary MBO FIFO model snapshots only factual references already present at activation. Later
same-price facts are behind the shadow order. Ahead executions, cancels, deletes and replaces reduce
queue-ahead, but queue depletion alone never fills. A subsequent displayed `E` or `C`, or a strict
trade-through, must supply causal fill evidence. `P`, `Q` and `B` never create displayed shadow fills.

## Strategy comparison

All three strategies implement the same quote interface and use the same observed factual levels,
latency scheduler, lifecycle, queue models, fee ledger and risk gates. The 10,000-row cross-language
oracle feeds identical state vectors to `symmetric_quote`, `avellaneda_stoikov` and
`signal_aware_as`. The inventory-pressure planted control then compares symmetric and A-S quote
responses under identical evidence and risk limits; this comparison is an implementation control,
not a market-performance result.

Parameters with empirical meaning carry `fitted_partition = "train"` and the frozen protocol hash.
The Python calibration entry point rejects any partition other than `train`; Round 3 complete-date
chronological split and purge rules remain authoritative. Future midpoints appear only in terminal
marks and post-fill markouts, never in a decision snapshot.

## Accounting and evaluation

The exact ledger uses integer nanodollars. One price4 unit is 100,000 nanodollars and one `mid2` unit
is 50,000 nanodollars per share. FIFO inventory lots produce realized gross PnL. Terminal midpoint
equity and conservative bid/ask liquidation equity are distinct, and fees/rebates are never folded
into gross PnL.

Post-fill markout at horizon `h` selects the last valid sample with
`(timestamp, sequence) <= fill_time + h`. Invalid boundaries yield missing values. Directional value
is positive when the future move favors the shadow fill; adverse-selection cost is its negative.
Whole-day block bootstrap is available for real multi-day analysis, but it is not applied to the
single synthetic session.

## Reproducibility

The complete parsed protocol is canonicalized and SHA-256 hashed. Audit rows have versioned schemas,
stable field order and LF line endings. The semantic digest excludes wall time, paths, filesystem
metadata and physical chunking. GCC/Clang and chunk-size checks compare canonical audit content.
