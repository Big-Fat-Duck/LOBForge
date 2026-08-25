# Round 3 dependency and license audit

The exact resolved graph is `python/uv.lock`; `python/pyproject.toml` is the direct-dependency
policy. The package is private and is not published by this work.

| Direct dependency | Locked family | Upstream license family | Purpose |
|---|---|---|---|
| NumPy | 2.4.6 | BSD-3-Clause | typed arrays/vectorized features |
| PyArrow | 23.0.1 | Apache-2.0 | strict schemas, streaming JSON and Parquet |
| SciPy | 1.17.1 | BSD-3-Clause | rank/Pearson statistics |
| scikit-learn | 1.9.0 | BSD-3-Clause | interpretable regression/logistic models and metrics |
| Matplotlib | 3.11.1 | PSF-compatible/BSD-style | static deterministic figures |
| pytest / pytest-cov | 9.1.1 / 7.1.0 | MIT | tests and branch coverage |
| Hypothesis | 6.165.10 | MPL-2.0 | property tests |
| Ruff | 0.15.6 | MIT | lint and formatting |
| mypy | 1.20.2 | MIT | strict static typing |

A fresh `uv 0.12.5 sync --frozen --all-groups` under CPython 3.11.9 installed the project and 33
resolved packages successfully. The temporary clean environment was removed after the check.

Transitive versions and hashes are resolved by uv, not copied into this document. No deep-learning,
notebook, distributed-compute, database, networking/service or market-data package was added. This
audit records software licensing only; no external market data was downloaded or relicensed.
