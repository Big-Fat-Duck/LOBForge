# LOBForge Round 4 validation report

## Verdict

Engineering gates E1-E15 are **PASS**. Round 4 is complete as an offline deterministic
counterfactual-engineering implementation. This verdict does not include real-market evidence:
F1-F3 and P1 are all `BLOCKED: DATASET_NOT_PROVIDED`.

The frozen protocol SHA-256 is
`ba491847e99df5479a21898ca64d1e986300176ebcab5e85c37b46b7f7bc2e74`. The primary synthetic
semantic digest is `49c0a04b17a0de32`.

## Engineering gates

| Gate | Status | Reproducible evidence |
|---|---|---|
| E1 | PASS | Linux GCC/Clang Release and Windows GCC 13 Release each passed 14/14 CTest. Existing Round 1/2 named tests passed 27/27 and 18/18 under sanitizers. Registered performance changes were -8.78% and -4.54%, both above the -10% floor. Existing book-event golden/digests remained stable. |
| E2 | PASS | `lob_mm` consumes read-only `FactualState` snapshots; local submit equality tests preserve factual canonical state. Strategy receives a copied `MarketSnapshot`. Round 1 matcher is never referenced by factual replay or shadow fill code. |
| E3 | PASS | Golden tests cover market (10 ns), compute (20 ns), order (30 ns), cancel (10 ns), replace (10 ns), factual-first timestamp ties, two-symbol interleaving and session end. Ten CLI runs produced one digest. |
| E4 | PASS | Typed tests traverse submitted, pending-ack, active, pending-cancel, cancelled, pending-replace, partially-filled, filled, rejected, session-ended and risk-cancelled states; pending-cancel fills, reprioritization and illegal transitions are asserted. Activation rejection paths cover quantity, inventory, open-order, notional, stale, halt, closed, close cutoff, missing/one-sided, locked/crossed, crossing and stop-switch reasons. |
| E5 | PASS | All three queue models pass goldens. Primary MBO FIFO matches an independent map oracle after every event for 20 seeds and 100,000 mutations. A failure reports seed plus the shortest deterministic prefix length and event fields. |
| E6 | PASS | C++ exact-ledger goldens plus Python Hypothesis oracle cover buys, sells, partial/multiple fills, FIFO realized PnL, cash, inventory, turnover, fee, rebate, midpoint and conservative liquidation equity, terminal/zero inventory and checked-overflow rejection. |
| E7 | PASS | 10,000 deterministic market/inventory/parameter states for all three strategies match the Python scalar oracle. Goldens cover equations, small/invalid gamma, invalid k, rounding, session time, one-sided/crossed states and inventory-side suppression. Fitted metadata is train-only and protocol-bound. |
| E8 | PASS | Non-bypass tests cover maximum inventory/order/open orders/notional, stale market, halt, closed session, close cutoff, maximum loss, drawdown, one-shot stop switch and forced cancellation. |
| E9 | PASS | The metric dictionary specifies formula, unit, denominator, missingness and causality. CLI goldens assert the complete primary ledger and 10 ms/100 ms/1 s markouts. An independent oracle tests right-continuous `(timestamp, sequence)` selection and invalid future boundaries. |
| E10 | PASS | C++ future-tail variants preserve the complete pre-cutoff audit while changing final digest. Python prefix/future perturbation, train-only fit/date scope, Round 3 global-date split and maximum-horizon purge tests all pass; validation/test fitting is rejected. |
| E11 | PASS | All eight deterministic controls pass. A-S time-weighted absolute inventory is 0.905 versus symmetric 5.0, an 81.9% planted reduction. Planted IC is 0.577275; null/shuffled ICs are -0.002116/-0.005602 and remain explicitly non-market evidence. |
| E12 | PASS | Ten runs and chunks 1/1,024/65,536 produce semantic digest `49c0a04b17a0de32`. GCC and Clang audit/protocol/summary files have identical content hashes. Protocol and fixed seed are recorded. |
| E13 | PASS | 14/14 CTest on three Release builds; Python 65/65; Round 4 aggregate branch coverage 93.89%; Ruff lint/format, strict mypy and C++ format pass. ASan/UBSan/LSan direct suites pass 27/27 + 18/18 + 11/11. The new fuzz target ran 31 s/257,383 executions and ITCH regression fuzz ran 31 s/307,824, with no crash, hang or sanitizer finding. |
| E14 | PASS | Full active-simulator median is 1,206,313 facts/s, p99 2.9 us and peak RSS 74,252,288 B. Five runs, warm-up, environment and earlier-round medians are recorded in the benchmark report. |
| E15 | PASS | Architecture/methodology, queue ADR, audit schemas, CLI/config/metric/synthetic/benchmark/validation/license documents and publication checklist exist. Tracked-file privacy, secret, restricted-data, path and artifact scans pass; ignore rules cover generated/private data. No license, visibility, commit or publication action occurred. |

## Test and reproducibility detail

- CTest: 14 target-level tests on each of Linux GCC 11.4, Linux Clang 14 and Windows GCC 13.1.
- Named C++ cases: Round 1 27, Round 2 18, Round 4 11; both Round 1 and Round 2 include independent 100,000-event/20-seed differential tests.
- Python: 65 tests; 15 focused Round 4 tests measure 93.89% aggregate branch coverage across five new core modules.
- Clean install: package 0.4.0 built and launched in a new Python 3.12 environment; project tests use the locked Python 3.11.9 environment.
- Dependency lock SHA-256: `318407ecb7c85f27d16ebeb86f99625cd4839d8dc4476190e46e2b32fb70bb04`; Round 4 added no runtime dependency.
- Cross-build byte SHA-256: orders `9df9838279d51e9f504193ba2443e4e4a6923d984128b3e965deab0005127fe7`; fills `c8279f55a63399bc8d09e0d263299d21940f1639f83228466707edd3a4a685d5`; inventory `c2f4894c2b6eff6c11def6ce5c1521396b5a662f69ec87f51826036043b90164`; summary `fcf23997ba78301f116aa7436aba58611ae0dbfd26f48139ea60e2b66cde5575`.
- Logical analysis digests: orders `adfd6b244304938b5f27ab523370b99d2cf5899e5d69695eb8783ddd825b0f27`; fills `88f1883beffb3fdbee14797ef60c2a1edead498df2102820d557ab00281ccf8f`; inventory `8edcee0622c3411e0d5f6f6e8be5c54eec7212658e03b15198c6f49ad2597ae3`.
- Two independent synthetic-report generations were byte-identical: metrics `c53ca3044a2a6f691a131f6c5bf1e2e4c1989047ad65590bbaf5c9478119537e`, Markdown `98478d5771d569e6fedcb3738022d1ac5c29ae51f826af4e770baf166fdc85bb`, PNG `56f8e005d2758eeb3113cc1d618277964e7f9de92b13fdc144da75214296bc36`.

The local Ubuntu 22.04/Clang 14 ASan runtime intermittently exited during interceptor startup before
`main` and without a code diagnostic. Immediate verbose retries initialized ASan and completed every
direct suite and CLI with leak detection enabled. This environment-only startup flake is disclosed;
no sanitizer finding was suppressed. The CI job uses a current hosted Ubuntu runtime and runs the
same flags from a clean process.

## Primary synthetic result

The C++ fixture applies 17 facts, submits two 10-share quotes, acknowledges both, records three
replacements and one 5-share partial buy fill. Terminal inventory is +5; cash is -5,000,000,000
nanodollars; rebate is 1,000,000; gross midpoint PnL is 125,000,000; net midpoint PnL is
126,000,000; conservative liquidation PnL is 49,500,000. Directional markouts are 75,000,000,
100,000,000 and 125,000,000 nanodollars at 10 ms, 100 ms and 1 s.

These values prove that the planted causal path, accounting and post-trade analysis agree. They do
not estimate real fill probability, market impact, alpha or profitability.

## Real-market evidence

| Evidence | Status | Reason |
|---|---|---|
| F1 - REAL_DATA_PROVENANCE | BLOCKED: DATASET_NOT_PROVIDED | No licensed, provenance-complete real session was supplied. |
| F2 - REAL_COUNTERFACTUAL_REPLAY | BLOCKED: DATASET_NOT_PROVIDED | A synthetic fixture cannot establish real replay behavior. |
| F3 - OUT_OF_SAMPLE_ROBUSTNESS | BLOCKED: DATASET_NOT_PROVIDED | There are no globally split real dates for final-test evaluation. |
| P1 - PROFITABILITY_EVIDENCE | BLOCKED: DATASET_NOT_PROVIDED | Synthetic PnL is a planted accounting control, not profitability evidence. |

## Prohibited conclusions

Round 4 does not prove real alpha, profitability, causality, production readiness, realizable fills,
deployability or real market impact. It contains no broker/exchange connection, live order route,
credentials or trading mode.
