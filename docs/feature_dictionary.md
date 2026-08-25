# Round 3 feature and label dictionary

All price inputs are integer Price(4). Price features exist only in a two-sided, non-crossed book
during `market_hours` with trading state `T`. Invalid, paused, halted, single-sided or crossed
states produce null and reset all path-dependent state. Zero is a valid numeric observation and is
never used as a missing-value sentinel.

| Column | Type | Definition and availability |
|---|---|---|
| `mid2` | nullable int64 | `best_ask_price4 + best_bid_price4`; exactly twice the midpoint |
| `spread_price4` | nullable int64 | `best_ask_price4 - best_bid_price4` |
| `imbalance_l1/l5/l10` | nullable float64 | `(sum_bid_qty_k-sum_ask_qty_k)/(sum_bid_qty_k+sum_ask_qty_k)`; null unless both sides have all `k` levels |
| `weighted_midprice_l1` | nullable float64 | `(ask*best_bid_qty + bid*best_ask_qty)/(best_bid_qty+best_ask_qty)` |
| `weighted_midprice_displacement` | nullable float64 | `weighted_midprice_l1 - mid2/2` |
| `ofi_l1_events_10/50/100` | nullable int64 | Sum of the most recent 10/50/100 L1 Cont increments, including the anchor increment |
| `ofi_l1_10ms/100ms/1s` | nullable int64 | Sum of L1 increments in `(t-window,t]` only |
| `stoikov_microprice` | nullable float64 | `mid2/2 + g(spread, train-fitted imbalance bin)` |
| `stoikov_microprice_displacement` | nullable float64 | Learned adjustment `g(state)` |
| `stoikov_fallback_reason` | nullable string | Explicit unseen/undersampled/invalid/pathological-state reason |

For best bid/ask `(b,a)` and quantities `(qb,qa)`, the static weighted midpoint obeys

```text
weighted_midprice_l1 - mid = spread * imbalance_l1 / 2
```

and lies within `[b,a]`. It is a static imbalance-weighted midpoint, also called
`static_microprice_proxy` in explanatory text. It is not the learned Stoikov micro-price. Given
spread, its displacement and L1 imbalance are algebraically collinear, so model code rejects their
unexplained simultaneous inclusion and the protocol specifies separate ablations.

The L1 OFI increment for consecutive valid states is

```text
1[Pb_n >= Pb_prev] Qb_n - 1[Pb_n <= Pb_prev] Qb_prev
- 1[Pa_n <= Pa_prev] Qa_n + 1[Pa_n >= Pa_prev] Qa_prev.
```

Event windows and clock windows are trailing and past-only. State resets on date/session changes,
halt/pause, the first valid state after resume, invalid/single-sided/crossed book, input gap/error,
truncation, and session end. Same-timestamp events are ordered by `sequence`.

Event-time targets are `target_mid2_delta_event_{1,10,50,100}` and their signed three-class
directions. Clock targets are `target_mid2_delta_clock_{10ms,100ms,1s}` and directions. A clock
state at `t` is the last `(timestamp_ns,sequence) <= t`; a label at `t+h` likewise never selects a
message after `t+h`. Labels do not cross a reset boundary or end of input. Zero changes remain
class 0.
