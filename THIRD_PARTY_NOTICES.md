# Third-party sources and dependency notices

This inventory supports the `v0.4.0-rc1` release audit. LOBForge's project license grant is in
`LICENSE`, and its copyright notice is in `NOTICE`; this file records separate third-party terms
and is not legal advice. The repository does not vendor a third-party C++ or Python library,
third-party source fixture, paper text, external image, or real market-data file. Python packages
are installed from their upstream distributions according to `python/uv.lock`.

## Research and protocol references

| Reference | How LOBForge uses it | Included or adapted material | Licensing note |
|---|---|---|---|
| Nasdaq TotalView-ITCH 5.0 specification, June 11, 2026 | Protocol field/layout and state-transition definitions | Independent decoder and synthetic literal-hex fixtures; no Nasdaq code or market data | Protocol reference only; no Nasdaq endorsement is claimed |
| Cont, Kukanov and Stoikov, *The Price Impact of Order Book Events*, arXiv:1011.6402 / JFE 2014 | Mathematical definition of L1 order-flow imbalance | Independent formula implementation and tests | No paper text, code or data copied |
| Stoikov, *The Micro-Price: A High-Frequency Estimator of Future Prices*, DOI 10.1080/14697688.2018.1489139 | Finite-state spread/imbalance concept | Independent train-only first-adjustment estimator | No paper text or data copied |
| `sstoikov/microprice`, audited at commit `4e5f29a843cb59f82bf3c64b0725642792048124` | Conceptual comparison only | No notebook code or CSV fixture included; local data structures and solver are independently implemented | Upstream repository had no root license at audit time, so its code/data must not be copied or redistributed here |
| Avellaneda and Stoikov, *High-frequency trading in a limit order book*, DOI 10.1080/14697680701381228 | Reservation-price and spread formula reference | Independent C++ formula implementation and independent Python oracle | No paper text, source implementation or data copied |
| NIST FIPS 180-4, *Secure Hash Standard* | SHA-256 algorithm definition for protocol/manifests | Independent implementation tested with known vectors | Public standard; no third-party cryptographic source copied |

The public column name `stoikov_microprice` is retained for schema compatibility. Documentation
qualifies it as a **Stoikov-style finite-state first-adjustment estimator**; it does not claim to
reproduce every recursion in the paper or public notebook.

## C++ toolchain

LOBForge's C++ runtime dependency is the C++20 standard library only. CMake, Ninja, GCC, Clang,
MinGW runtimes, sanitizer runtimes and libFuzzer are build/test tools and are not vendored or
redistributed by this source repository. Their licenses apply when a user installs those tools or
distributes compiled binaries.

## Python dependency inventory

The lock contains hashes and exact marker-specific resolutions. The table records the reviewed
license family from installed distribution metadata or upstream package metadata; a distributed
wheel may include additional bundled-component notices that must travel with that wheel.

| Package group | Packages | Reviewed license family |
|---|---|---|
| Direct runtime | NumPy | BSD-3-Clause plus bundled 0BSD/MIT/Zlib/CC0 components |
| Direct runtime | PyArrow | Apache-2.0; binary distributions may contain additional NOTICE material |
| Direct runtime | SciPy | BSD-3-Clause; wheels may bundle OpenBLAS/LAPACK and compiler-runtime notices |
| Direct runtime | scikit-learn | BSD-3-Clause |
| Direct runtime | Matplotlib | PSF-compatible Matplotlib license; bundled data have their own notices |
| Direct development | pytest, pytest-cov, Ruff, mypy | MIT |
| Direct development | Hypothesis | MPL-2.0 |
| Build backend | Hatchling 1.27.0 | MIT; exact build constraint is recorded in `python/pyproject.toml` |
| Transitive | colorama, contourpy, cycler, joblib, kiwisolver, scikit-learn helpers, threadpoolctl | BSD-family |
| Transitive | coverage | Apache-2.0 |
| Transitive | fonttools, iniconfig, librt, mypy-extensions, narwhals, pluggy, pyparsing, six, tomli | MIT-family |
| Transitive | packaging | Apache-2.0 OR BSD-2-Clause |
| Transitive | pathspec | MPL-2.0 |
| Transitive | Pillow | MIT-CMU |
| Transitive | Pygments | BSD-2-Clause |
| Transitive | python-dateutil | Apache-2.0 OR BSD-3-Clause |
| Transitive | sortedcontainers | Apache-2.0 |
| Transitive | typing-extensions | PSF-2.0 |
| Build transitive | trove-classifiers | Apache-2.0 |

Marker-specific lock branches may contain more than one NumPy or SciPy version for supported
Python/platform combinations. `python/uv.lock`, not this summary, is authoritative for names,
versions, artifact URLs and hashes.

If wheels, executables or containers are distributed later, collect the actual `LICENSE`,
`COPYING` and `NOTICE` files from every shipped distribution. This source-only inventory is not a
substitute for a binary-distribution notice bundle.

## GitHub Actions

| Action | Pinned commit | Tag verified during audit | License |
|---|---|---|---|
| `actions/checkout` | `11bd71901bbe5b1630ceea73d27597364c9af683` | v4.2.2 | MIT |
| `actions/setup-python` | `a26af69be951a213d495a4c3e4e4022e16d87065` | v5.6.0 | MIT |

The workflow also installs Ubuntu packages and `uv==0.12.5` at runtime. Those artifacts are not
stored in this repository. Shared runner images and package indexes are external supply-chain
inputs, so a local run cannot establish that a future hosted run uses identical infrastructure.

## LOBForge project-license status

LOBForge is licensed under Apache License 2.0. The root `LICENSE` contains the complete terms,
`NOTICE` records `Copyright 2026 Haoxiang Sang`, and `python/pyproject.toml` uses the SPDX expression
`Apache-2.0`. This project license does not relicense the dependencies and references inventoried
above; their own licenses and notices continue to apply.
