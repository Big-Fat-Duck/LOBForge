# Round 3 methodology

## Scope and authoritative boundary

Round 3 tests research-data and evaluation correctness. C++ remains the only ITCH parser and
factual book implementation. Python consumes versioned NDJSON, validates it, writes fixed-schema
Parquet in bounded batches, and computes research artifacts. The interface favors auditability and
subprocess isolation; it is not a claim about production transport latency.

The immutable chain is:

```text
ITCH bytes -> C++ decoder/factual reconstruction -> book_event/v1 NDJSON
           -> strict Python validation -> canonical logical rows + Parquet
           -> features -> boundary-safe labels -> chronological evaluation
```

The source SHA-256 and size, C++ factual digest, protocol/lock digests, schemas, arguments, logical
partition digests and whole-dataset semantic digest are recorded. Semantic digests cover typed
Arrow logical columns and are independent of Parquet metadata, batch size, temporary directory and
absolute path.

## Features and learned micro-price

Static features follow `docs/feature_dictionary.md`. The learned micro-price state is
`(spread_price4, imbalance_bin)`. Quantile bin edges, state universe, no-mid-change matrix `Q`,
absorbing mid-change matrix `R`, conditional price-change counts and optional smoothing use train
only. The expected adjustment solves `(I-Q)g=r` with `numpy.linalg.solve`; no explicit inverse is
formed. Row probability error, sample count, reachability through matrix conditioning, condition
number and solve residual are diagnostics. An unseen or undersampled state, singular/ill-conditioned
system or failed residual check gives null plus a reason. Validation and test call only `transform`.

This is intentionally distinct from the static weighted midpoint. The implementation follows the
finite-state ideas in Stoikov’s paper and public reference code:
<https://doi.org/10.1080/14697688.2018.1489139> and
<https://github.com/sstoikov/microprice>.

OFI follows Cont, Kukanov and Stoikov’s L1 definition:
<https://arxiv.org/abs/1011.6402>. Prediction uses lagged OFI in `(t-100ms,t]`; it does not relabel
contemporaneous OFI/return association as forecasting.

## Sampling, frozen protocol and splitting

`configs/round3_protocol.toml` freezes universe rules, the 100 ms primary hypothesis, feature
ablations, global chronological date split, purge, models/search space, seed, block bootstrap and
success criterion. `lobforge-research freeze-protocol` emits its exact-byte SHA-256, which every
final-test artifact records.

Complete dates are sorted globally and split approximately 70/15/15. Every symbol from one date is
in the same partition. At each split boundary the maximum configured lookback/lookahead (1 second)
is purged by absolute event time. Random row splitting is absent. Symbol selection, imputation,
scaling, imbalance bin edges, micro-price transitions, regression/classification coefficients,
calibrator and any threshold fit on train only. The final chronological tail of train is reserved
for probability calibration. Validation selects the registered model/hyperparameter; final test is
evaluated once.

## Models, metrics and inference

Baselines are zero-change regression, empirical prior, majority class and the signs of L1
imbalance, weighted-mid displacement, and lagged OFI. Models are univariate linear/logistic
ablations plus regularized linear/logistic combinations. Algebraically redundant imbalance and
weighted-mid displacement cannot enter one model through the public constructor.

Regression reports MAE, RMSE, out-of-sample R² relative to the zero-change baseline, Pearson and
Spearman correlations. Direction is three-class `{-1,0,+1}` and reports balance, confusion matrix,
macro-F1, balanced accuracy, MCC, log loss, multiclass Brier and one-vs-rest AUC. Raw accuracy is
secondary. Calibration reports Brier, ten equal-frequency confidence bins, ECE, maximum error and
before/after tables.

Metrics are first calculated by symbol-day and pooled with equal symbol-day weight. Confidence
intervals resample whole symbol-day blocks with a fixed seed; adjacent rows are never treated as
IID. Secondary features/horizons use Benjamini–Hochberg FDR at 5% and are labelled exploratory.

## Controls and admissible conclusions

The positive generator makes a future change depend on already-visible OFI/imbalance. An
independent null generator samples labels independently, and a separate deterministic label shuffle
must leave the feature digest unchanged. These controls validate implementation sensitivity and
specificity only.

Engineering gates C1–C13 are separate from real evidence D1–D3 and hypothesis H1. H1 is evaluated
only if provenance/license, a strict complete-session replay and the registered 20-day/5-symbol/100
symbol-day coverage all pass. “SUPPORTED” requires positive equal-weight mean test IC, a positive
whole-day bootstrap lower bound and at least 60% positive test symbol-days. Otherwise it is
`NOT_SUPPORTED`; if data evidence is blocked, H1 is `BLOCKED`. No Round 3 outcome proves causality,
executable alpha, NBBO validity, fills, profitability or risk-adjusted return.
