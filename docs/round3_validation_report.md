# Round 3 validation report

## Result and scope

All engineering gates C1-C13 pass on the license-safe synthetic fixtures and local toolchains.
Real-market evidence does not pass because no licensed dataset was provided: D1 is
`BLOCKED: DATASET_NOT_PROVIDED`, D2 and D3 are `BLOCKED`, and H1 is `BLOCKED`. Synthetic results
validate data/evaluation mechanics only. They do not prove market predictability, causality,
execution quality, NBBO validity, fills, alpha or profitability.

The frozen protocol SHA-256 is
`0315c2c762380ceb5ac30816f5112455fe6aec2f87b089634df78c379eecb2e4`.

## Engineering gates

| Gate | Status | Executed evidence |
|---|---|---|
| C1 | PASS | Windows GCC Release and Linux GCC/Clang Release: 11/11 CTest each; Clang ASan/UBSan/LSan: 11/11; libFuzzer 30 s: 169,833 executions, no finding; Round 1/2 performance regressions +6.46%/-4.13%. |
| C2 | PASS | `book_event/v1` byte-golden, fixed key/LF order, mandatory date/depth, version/date rejection and stdout/stderr integration tests. GCC/Clang exporter bytes matched. |
| C3 | PASS | Streaming subprocess ingest, scalar and Arrow validators, fixed schemas, atomic directory publication, Parquet partitions, source/protocol/lock digests, stable exit-code negative tests. |
| C4 | PASS | Exact `mid2`/spread, L1/L5/L10 imbalance and weighted-mid golden/property tests; train-only Stoikov chain analytic, direction, serialization and failure-reason tests. |
| C5 | PASS | Cont L1 OFI golden cases, timestamp/sequence/reset/multi-symbol tests and 100,000-state independent scalar oracle comparison. |
| C6 | PASS | Event 1/10/50/100 and right-continuous clock 10 ms/100 ms/1 s labels; same-timestamp sequence, invalid boundary and half-tick tests. |
| C7 | PASS | Prefix invariance, future-tail perturbation, feature sequence bound, global-date split, one-second purge, fit-scope and target/future/next/lead denylist tests. |
| C8 | PASS | Required baselines; five linear/logistic ablations; train-tail calibration; symbol-day metrics; whole-block bootstrap; BH FDR; positive/null/shuffled controls. |
| C9 | PASS | Deterministic full synthetic report validates rows, schema, predictions and artifacts; the small fixture pins exact metrics/model/prediction logical SHA-256 values. |
| C10 | PASS | Ten alternating-batch logical-digest runs; 1,024/65,536 batch equivalence; GCC/Clang byte/logical equivalence. Exporter SHA-256: `154a41ba6f4c8890a30c5a5d8778ad1ca1a7b1593872dc6572d9e812fbfcce50`. |
| C11 | PASS | 50 pytest/Hypothesis tests; selected core branch coverage 96.01%; Ruff lint/format and strict mypy pass; CI includes Release matrix, sanitizer, 30 s fuzz and clean Python 3.11 lock install. |
| C12 | PASS | Features 4,927,203 rows/s; pipeline 309,378 rows/s; 1,000,000-row peak RSS 1,185,189,888 B; Round 1/2 thresholds pass. |
| C13 | PASS | README, schema, dictionary, methodology, leakage audit, validation/benchmark reports, locked graph and dependency/license audit present; generated/market/model artifacts and virtual environments are ignored. |

## Deterministic interface and dataset evidence

The C++ fixture replay produced seven displayed-book mutation records from 28 framed records. GCC
and Clang emitted byte-identical NDJSON and the factual-book summary digest was
`d3f11ab4fe308743`. Python validates every row rather than skipping errors. The semantic digest is
defined over canonical typed logical rows, not Parquet bytes; paths, system time, temp names,
metadata ordering and unordered-container iteration are excluded.

An end-to-end C++ fixture build at batch sizes 1,024 and 65,536 produced the same dataset semantic
SHA-256, `205b2aece94ab985d2878e0443c34e4a9083f201d23295947825aa6a590a5d74` (seven book events and
five valid research rows).

Malformed JSON, missing summary, truncated streams, wrong schema/version, backward sequence,
illegal nullability/ranges and failed subprocesses return stable nonzero outcomes without a final
published directory. The same golden stream passes both the independent scalar validator and the
vectorized Arrow validator.

## Synthetic research evidence

The fixed positive fixture contains 20 dates, five symbols and 50,000 rows. The chronological split
is 14/3/3 dates (35,000/7,500/7,500 rows), with no synthetic boundary row requiring purge. On the
final synthetic test only:

- equal-weight symbol-day lagged-OFI linear regression: MAE 0.83378, RMSE 1.02099,
  out-of-sample R2 0.66712, Pearson 0.81717, Spearman 0.86095;
- equal-weight symbol-day lagged-OFI three-class logistic: balanced accuracy 0.74685, macro-F1
  0.74657, MCC 0.67353;
- zero-return prevalence: 0.21613;
- equal-weight symbol-day IC: 0.86095, 100% positive symbol-days, whole-symbol-day bootstrap 95% CI
  [0.85495, 0.86606];
- calibration ECE: 0.00907 before and 0.00782 after train-tail calibration;
- null fixture: Spearman 0.00223 and nonzero binary balanced accuracy 0.49787.

The positive coefficient, >=0.10 IC, >=0.60 balanced accuracy, monotone-decile, learned-microprice
direction, null IC/accuracy, shuffle invariance and scope-language checks all pass. This was planted
by construction and therefore cannot be cited as empirical market evidence. The generated report
records metrics SHA-256
`0526e78fad5a801d36198c689adddc5c6978c6962972e535b5246d7d96ac77d0`, model SHA-256
`de0ff5901c99659c5bc4b9fb38fc981e3eecfa3d7f910fca4f382b066e0c691f`, and prediction logical
SHA-256 `ee9da6ac81bd295adbaa0c66a17072d48dfd07a8a8166285b9cf37466c289d1b`.

## Real-data evidence status

| Evidence | Status | Reason |
|---|---|---|
| D1 - provenance | BLOCKED: DATASET_NOT_PROVIDED | No provider, license, venue/session, size, SHA-256 or completeness record can exist without a supplied lawful dataset. |
| D2 - real replay smoke | BLOCKED | No lawful complete Q-to-M session is available for strict replay. |
| D3 - empirical coverage | BLOCKED | No 20-day/5-symbol/100-symbol-day real universe is available. Thresholds were not reduced. |
| H1 - primary signal | BLOCKED | Frozen final-test hypothesis is not evaluated until D1-D3 all pass. |

## Limitations

NDJSON is the auditable Round 3 subprocess boundary, not a low-latency production transport. The
work deliberately excludes quoting, order generation, fill/queue models, latency, fees, inventory,
PnL/risk statistics, live protocols, broker connectivity, deep learning, dashboards and databases.
Those omissions prevent any inference about executable economics even after a future statistical
association test.
