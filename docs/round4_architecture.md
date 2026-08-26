# Round 4 architecture and methodology

## Domain boundary

```text
ITCH bytes -> safe decoder -> factual FactualState (exchange truth, immutable to Round 4)
                               |
                               +-> post-apply BookMutation + read-only depth
                                      |
                                      v
                            deterministic shadow scheduler
                         latency -> lifecycle -> queue evidence
                                      |
                strategy quote intent + risk gate (never factual mutation)
                                      |
                           shadow fills -> integer ledger
                                      |
                 versioned audit files -> Python independent analysis
```

Round 1, Round 2 and Round 4 are distinct libraries. No ITCH trade is submitted to the Round 1
matcher. No shadow quantity enters `FactualState`. The strategy API consumes a copied
`MarketSnapshot`; it has no mutable factual handle. The boundary is tested by factual canonical
state equality around local actions.

## Deterministic scheduling

The scheduler key is `(effective_timestamp_ns, factual_sequence, event_category, local_sequence)`.
Market receipt delay creates an observation; compute delay creates a decision; order/cancel/replace
delays create their corresponding confirmation. When factual messages share a timestamp, the
simulator defers all local events at that timestamp until the feed advances, so every fact wins the
tie. No wall clock, threads, unordered output iteration or random scheduling is used.

## Strategies

- `symmetric_quote` quotes a configured half-spread around factual midpoint.
- `avellaneda_stoikov` uses
  `reservation = midpoint - inventory * gamma * sigma_squared * time_remaining` and
  `spread = gamma * sigma_squared * time_remaining + 2/gamma * log1p(gamma/k)`.
- `signal_aware_as` adds `signal_coefficient * causal_signal` to reservation as a named ablation.

Bid intentions round down and asks round up to ticks, then select only a factual observed level
within maximum distance. Crossed, locked, one-sided, stale, halted, closed and close-cutoff states
suppress quotes. Invalid `gamma`/`k` reject the calculation; the small-gamma limit is `2/k`.
Python calibration in `round4_calibration.py` accepts only a literal `train` partition and stores
fit timestamps and the frozen protocol hash. Validation/test transforms cannot update parameters.

## Exact accounting and evidence scope

Price(4) is converted exactly to nanodollars (`1 price4 = 100,000 nanodollars`); `mid2` uses 50,000
nanodollars. Inventory, cash, fees, rebates and PnL use checked signed 64-bit values. FIFO inventory
lots define realized gross PnL. Midpoint and conservative bid/ask liquidation equities are both
reported; the latter charges the configured liquidation fee.

Synthetic controls test mechanics and planted relationships. They do not model the strategy's
effect on other participants, hidden liquidity, actual routing, latency distribution, venue fees,
production operations or deployability. Real provenance, replay robustness, out-of-sample evidence
and profitability remain separate F1-F3/P1 gates.
