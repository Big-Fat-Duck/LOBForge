# Round 3 benchmark methodology and results

## Environment and method

The full workload ran locally on an AMD Ryzen 7 5800H, Windows 10 10.0.19045, 15.35 GiB RAM,
Python 3.11.9, NumPy 2.4.6 and PyArrow 23.0.1. C++ Release regression checks used GCC 13.1 on
Windows; cross-compiler validation used GCC 13.3 and Clang 18 on a temporary Ubuntu 24.04 WSL1
environment. Inputs were generated before the measured interval. Each Python benchmark performed
one warm-up and three measured runs; the median is authoritative. Peak RSS is the process high
water mark, so it is deliberately conservative.

The feature workload transforms 1,000,000 pre-generated L1/L5/L10 states with the vectorized
mid/spread, imbalance and weighted-mid implementation. The pipeline workload processes 1,000,000
pre-generated NDJSON book-event rows through batch JSON parsing, fixed Arrow validation, canonical
logical digesting and Zstandard Parquet writing at batch size 65,536. Feature/label throughput is
reported separately; the pipeline number does not pretend to include a fitted model.

## Results

| Workload | Runs (seconds) | Median | Throughput | Peak RSS | Gate |
|---|---|---:|---:|---:|---|
| Vectorized feature transform, 1,000,000 rows | 0.202955, 0.228759, 0.196931 | 0.202955 s | 4,927,203 rows/s | 630,022,144 B | PASS (>=1,000,000 rows/s) |
| NDJSON -> Arrow validation -> logical digest -> Parquet, 1,000,000 rows | 2.882291, 3.232294, 3.360567 | 3.232294 s | 309,378 rows/s | 1,185,189,888 B | PASS (>=100,000 rows/s; <=1.5 GiB) |

The pipeline digest was stable across all measured runs:
`ae93b72426d0613aeec2a864714bed261dfc551c7162c7b95b97438545be0ba6`.

## Earlier-round regression

The same host and benchmark definitions were used against the recorded pre-Round-3 medians.

| Benchmark | Recorded baseline median | Round 3 median | Change | Gate |
|---|---:|---:|---:|---|
| Round 1 `mixed` | 4,893,861 commands/s | 5,210,018 commands/s | +6.46% | PASS |
| Round 2 `decoder_apply` | 4,760,340 messages/s | 4,563,532 messages/s | -4.13% | PASS |

Both remain above the registered -10% floor. The Round 2 mutation observer is disabled unless the
research exporter requests it, which keeps normal replay overhead bounded. Shared-runner CI uses
reduced smoke loads; these full local thresholds are the release evidence.
