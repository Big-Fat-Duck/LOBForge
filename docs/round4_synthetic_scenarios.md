# Round 4 deterministic synthetic scenarios

The Python command below writes `metrics.json`, a report and a compact plot. Every assertion is an
implementation control, not evidence of real alpha, fills or profitability.

```sh
cd python
uv run lobforge-research round4-synthetic-report --output ../artifacts/round4_controls
```

1. **No eligible fill:** same-price evidence under `trade_through_only` yields zero fills, inventory,
   cash, fees and PnL.
2. **Exact FIFO:** two ahead references experience partial execution, cancellation/deletion, then a
   behind execution produces partial/full shadow fills; replacement starts behind a fresh snapshot.
3. **Order latency race:** activation strictly before a fact can fill; an order effective after it
   misses; an equal timestamp also loses because facts win ties.
4. **Cancel latency race:** an execution between cancel request and confirmation remains fillable.
5. **Inventory pressure:** identical planted buy/sell evidence is fed to symmetric and A-S quote
   oracles; inventory-induced one-sided distance makes A-S time-weighted absolute inventory fall
   by more than the preregistered 20%.
6. **Adverse selection:** known 10ms/100ms/1s midpoints give exact signed markouts and the opposite
   adverse-selection costs via right-continuous selection.
7. **Fee shock:** identical fills preserve gross PnL and reduce net PnL by exactly the fee difference.
8. **Signal controls:** 50,000 fixed-seed observations contain independent null, shuffled and planted
   targets. The planted IC exceeds 0.50 while null/shuffled absolute IC stay below 0.02; market alpha
   status remains `NOT_EVALUATED_SYNTHETIC_ONLY`.

The C++ fixture additionally traverses ITCH factual reconstruction, delayed strategy observation,
activation, queue-ahead depletion, a partial shadow fill, exact ledger updates, future price moves,
session cleanup and versioned artifact publication. Its semantic digest is cross-compiler tested.
