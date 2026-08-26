# Round 4 metric dictionary

All money is integer nanodollars; quantity is shares; time is nanoseconds. A ratio with denominator
zero is null. Future-dependent metrics are analysis-only and never enter a strategy decision.

| Metric | Formula / denominator | Boundary and causality |
|---|---|---|
| Gross PnL | trade cash + inventory x terminal `mid2` x 50,000 | Null when no terminal valid sample exists; conservative equity is separate. Analysis-only terminal mark. |
| Fees / rebates | sum per-share configured integer x fill quantity | Zero with no fills; causal at fill. |
| Net PnL | gross PnL - fees + rebates | Exact integer; terminal future state required. |
| Conservative liquidation PnL | cash + long inventory x bid or short inventory x ask - fees + rebates - liquidation fee | Requires terminal two-sided book; analysis-only. |
| Submitted/acknowledged/cancelled/replaced/filled orders | typed lifecycle counts | Filled order means zero remaining; partial fill is a fill event but not a filled order. |
| Filled quantity / turnover | sum fill quantity | Turnover is unsigned absolute traded shares. |
| Order fill rate | fully filled orders / acknowledged orders | Null if no acknowledgements. |
| Quantity fill rate | filled quantity / submitted quantity | Submitted quantity includes rejected requests; null if none. |
| Spread capture | sum `side_direction * (anchor_mid2 - 2*fill_price4) * 50,000 * qty` | Buy direction +1, sell -1; anchor is causal pre-mutation midpoint. |
| Realized spread at horizon h | `2 * directional_markout_h / eligible_quantity`, nanodollars/share | Summary records both `eligible_quantity` and the ratio; null when quantity is zero; uses future state. |
| Time-weighted absolute inventory | integral abs(inventory) dt / eligible market time | Null denominator represented by zero integral plus explicit eligible time. |
| RMS inventory | sqrt(integral inventory squared dt / eligible market time) | Same eligibility/reset boundary. |
| Inventory utilization | maximum abs inventory / configured limit | Null when limit is zero (invalid protocol). |
| Maximum drawdown | max(previous peak net midpoint equity - current net midpoint equity) | Causal running risk value; never floored at zero PnL. |
| Quote online rate | time with active/partial/pending-cancel/pending-replace exposure / eligible market time | Pending acknowledgement is not online; null if no eligible time. |
| Cancel/replace rate | (cancelled + replaced lifecycle events) / acknowledged orders | May exceed one after multiple refreshes; null if no acknowledgements. |
| Directional markout h | `direction * (mid2(fill+h)-2*fill_price4) * 50,000 * qty` | Buy +1, sell -1. Last `(timestamp, sequence) <= fill+h`; invalid boundary or absent horizon is missing. Positive favors the fill. |
| Adverse-selection cost h | negative directional markout h | Positive means adverse movement. |
| Latency/queue/fee sensitivity | stable table of identical fixture/protocol ablations | Never pooled across changed fill paths without labeling. |
| Risk suppressions / stop triggers | typed reason counts | Causal and integer; a stop transition occurs once. |

Sharpe ratio is intentionally absent from synthetic evidence. Without sufficient independent real
days it would be a misleading IID-style statistic.
