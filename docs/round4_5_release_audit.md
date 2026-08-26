# Round 4.5 local release audit

## Decision

**Status: `LOCAL_RELEASE_AUDIT_PASS`**

The planned `v0.4.0-rc1` source set is ready for an owner-reviewed commit and push to a private
repository. This is a source-content gate: private commit, candidate-tag, push and hosted-CI status
are verified by the separate release operation. It does not authorize public visibility, package
publication, a final `v0.4.0` tag or a GitHub Release.

F1–F3 and P1 remain `BLOCKED: DATASET_NOT_PROVIDED`. Nothing in this audit establishes real fills,
alpha, profitability, production readiness or live-trading readiness.

## Release scope and privacy

The authoritative count, byte total, largest file, file hashes and canonical digest are in
[`release_manifest.json`](../artifacts/round4_5/release_manifest.json). The manifest digest is not
duplicated here because this report is itself a hashed release file; avoiding that recursive
dependency makes regeneration well defined. The planned set contains 131 entries: 130 ordinary
files with byte counts and SHA-256 values, plus one generated-manifest sentinel whose physical
self-hash is deliberately excluded to avoid recursive hashing. The baseline has 122 tracked files
and the candidate adds nine source, legal and documentation files. Generated build trees, virtual
environments, caches, raw market/research data, fuzz corpora and benchmark outputs are excluded.

The scanner inspected the planned worktree, the complete Git index and all local Git blobs,
including unreachable objects. It found no high-confidence secret, private key,
credential-bearing remote URL, private email, phone number, restricted market-data file or file
over 10 MiB. The planned worktree and final staged index have zero absolute-path findings and zero
broken or case-mismatched Markdown links. Sixteen non-secret absolute build-path matches remain in
older history versions of two reports; the candidate versions replace them with portable commands.
History was not rewritten, as required by the audit scope.

## Fresh validation

### Release builds and tests

| Environment | Configuration | Result |
|---|---|---|
| Ubuntu 22.04 on WSL2, GCC 11.4 | Release, `-O3 -DNDEBUG` | 14/14 CTest passed in 4.66 s |
| Ubuntu 22.04 on WSL2, Clang 14.0 | Release, `-O3 -DNDEBUG` | 14/14 CTest passed in 4.54 s |
| Windows 10 build 19045, MinGW GCC 13.1 | Release, Ninja | 14/14 CTest passed |
| Independent clean Windows export, MinGW GCC 13.1 | Release, Ninja | 36/36 build targets and 14/14 CTest passed in 6.52 s |

The three direct C++ binaries contain 56 named tests: Round 1 has 27, Round 2 has 18 and Round 4
has 11. Their randomized oracles exercised 100,000 matcher commands, 100,000 factual-book
mutations and 100,000 queue-model states respectively. C++ formatting passed with clang-format 14
and 17. An initial Windows runner selected an obsolete GCC 6.3 installation and lacked the GCC 13
runtime on `PATH`; selecting the audited GCC 13.1 toolchain resolved this environment issue without
a source workaround.

### Python

The frozen Python 3.11.9 environment was created with uv 0.12.5 and 35 locked packages, including
the fixed Hatchling 1.27.0 build backend. Results:

- pytest/Hypothesis: 65 passed; the C++ exporter integration test ran and none were skipped.
- Selected ten Round 3/4 core modules: 95.16% aggregate branch coverage, above the 90% gate.
- Ruff lint and format-check: 34 files passed.
- strict mypy: 22 source files passed.
- Installed CLI version: `lobforge-research 0.4.0rc1`.

The coverage percentage applies only to the ten modules named in the workflow and claims index; it
is not whole-package or C++ coverage.

### Sanitizers and fuzzing

A fresh Clang 14 Debug build used ASan and UBSan with leak detection, halt-on-error and stack traces.
Valid runs that completed ASan initialization produced no ASan, UBSan or LSan finding:

- Round 1: 27/27, including the 100,000-command differential oracle.
- Round 2: 18/18, including all 23 message types and the 100,000-mutation oracle.
- Round 4: 11/11.
- Round 4 CLI contract: passed with semantic digest `49c0a04b17a0de32`.

WSL2 with Clang 14 intermittently raised `SIGSEGV` before ASan printed its initialization-complete
marker when many short child processes launched. The CLI contract required 14 whole-suite attempts
before all children initialized; the MM fuzz target required three launches. These pre-initialization
events contained no sanitizer diagnostic or code stack. They are recorded as a runner/runtime
limitation, not silently counted as successful tests. Every accepted run completed initialization,
returned success and had no sanitizer finding.

The two 30-second libFuzzer campaigns were kept separate:

| Target | Executions | Coverage/features | Peak RSS | Finding |
|---|---:|---:|---:|---|
| ITCH framing/decoder/permissive replay | 234,078 | 862 / 2,289 | 443 MiB | none |
| Round 4 shadow simulator | 225,308 | 472 / 3,005 | 427 MiB | none |

The execution counts are host-dependent and absence of a finding is not proof of memory safety.

### Performance and determinism

The main benchmark used an AMD Ryzen 7 5800H, Ubuntu 22.04/WSL2 limited to two logical CPUs and
about 2 GiB RAM, GCC 11.4, Release `-O3 -DNDEBUG`, 100,000 warm-up events and five pre-generated
1,000,000-event runs:

| Run | Facts/s | p99 latency |
|---:|---:|---:|
| 1 | 1,593,014.441 | 2.8 us |
| 2 | 1,612,653.525 | 2.8 us |
| 3 | 1,633,366.943 | 2.8 us |
| 4 | 1,633,635.376 | 2.8 us |
| 5 | 1,606,400.929 | 2.8 us |
| **Median** | **1,612,653.525** | **2.8 us** |

Peak RSS was 74,203,136 bytes. The median is 33.685% above the historical Round 4 measurement, so
there is no performance regression; the new public claim uses the complete fresh median rather
than a best run. Round 1 and Round 2 smoke workloads also exited successfully. Their machine-local
rates are retained only as regression evidence, not combined with the formal Round 4 measurement.

Ten independent primary simulations produced one semantic digest,
`49c0a04b17a0de32`, and one byte-identical summary hash. GCC and Clang across event chunks 1, 1,024
and 65,536 produced the same semantic digest and protocol digest. The analyzed orders, fills,
inventory and source-analysis logical digests were also identical across all six combinations.

### Clean source export

A temporary export was assembled from exactly all 131 manifest entries, not from `HEAD` and not
from ignored workspace content. The exporter automatically verified 130 ordinary file hashes and
the one manifest sentinel policy before copying. In that isolated copy:

- Windows GCC 13.1 Release configured and built 36/36 targets; CTest passed 14/14.
- A new frozen Python environment installed all 35 packages.
- Ruff, format-check, strict mypy and all 65 pytest/Hypothesis cases passed; branch coverage was
  95.16%.
- The README synthetic quickstart produced and analyzed semantic digest
  `49c0a04b17a0de32`; the deterministic control report returned `PASS`.

The export was containment-checked and only that newly created temporary directory was removed.
No workspace source, ignored user artifact or user data was deleted.

## CI and supply chain

The workflow has top-level `contents: read`, full-commit pins for official checkout/setup-python
actions, `persist-credentials: false`, no `pull_request_target`, no secret input, no ignored
failures and no artifact upload. Release GCC/Clang, Python, sanitizer, separate fuzz, format and
release-audit jobs are represented. Local workflow-equivalent commands pass, so the truthful
status recorded by this document is `LOCAL_CI_VALIDATED`; a `GITHUB_ACTIONS_GREEN` claim requires a
separate hosted run tied to the exact candidate commit.

`THIRD_PARTY_NOTICES.md` inventories direct, transitive and build dependencies, GitHub Actions,
the ITCH specification and academic/reference sources. No vendored third-party source or real
market data was identified. The cited `sstoikov/microprice` repository has no license; no notebook,
CSV or source from it is included, and the local model is described narrowly as a Stoikov-style
finite-state first-adjustment estimator.

LOBForge is licensed under Apache License 2.0. The root `LICENSE` contains the complete terms and
application notice, `NOTICE` records `Copyright 2026 Haoxiang Sang`, and
`python/pyproject.toml` registers the SPDX expression `Apache-2.0`. The project license does not
relicense any third-party dependency or reference.

## RA1–RA12

| Gate | Status | Evidence | External action required |
|---|---|---|---|
| RA1 — workspace and scope | PASS | Manifest file set; expanded ignore rules; no user data removed | No |
| RA2 — portable paths and links | PASS | Planned worktree: 0 absolute paths; Markdown/link/case findings: 0 | No; old non-secret paths remain only in history |
| RA3 — secrets and privacy | PASS | Worktree, final index, remote URLs and all local Git blobs scanned; secret findings: 0 | GitHub Security review is separate |
| RA4 — data and large files | PASS | Restricted-data findings: 0; release files over 10 MiB: 0 | No |
| RA5 — third-party sources and license | PASS | Apache-2.0 `LICENSE`; `NOTICE`; SPDX metadata; `THIRD_PARTY_NOTICES.md` | No |
| RA6 — traceable claims | PASS | `docs/claims_and_evidence.md`; synthetic/real and historical/fresh scope separated | No |
| RA7 — public README | PASS | Public entry page, quickstart, architecture, evidence, policy, limits and roadmap | Verify GitHub rendering after private push |
| RA8 — CI security and completeness | PASS | Pinned/minimal workflow and locally passing equivalents; `LOCAL_CI_VALIDATED` | Hosted run is verified separately per commit |
| RA9 — clean source export | PASS | Isolated C++/Python/static-check/quickstart validation | No |
| RA10 — engineering regression | PASS | Fresh Release, 65 pytest, sanitizer, two fuzz targets, benchmarks and determinism | No |
| RA11 — manifest and reproducibility | PASS | Strict manifest self-validation; 130 hashes + 1 sentinel; ten canonical generations have one digest | No |
| RA12 — release decision | PASS | RA1–RA11 pass; no untreated secret, restricted data or false public claim | Private release operation is tracked separately; public release remains prohibited |

## External evidence boundary

| Gate | Status |
|---|---|
| F1 — real-data provenance | `BLOCKED: DATASET_NOT_PROVIDED` |
| F2 — real counterfactual replay | `BLOCKED: DATASET_NOT_PROVIDED` |
| F3 — out-of-sample robustness | `BLOCKED: DATASET_NOT_PROVIDED` |
| P1 — profitability evidence | `BLOCKED: DATASET_NOT_PROVIDED` |

## Reproduction entry points

```sh
python tools/release_audit.py \
  --manifest artifacts/round4_5/release_manifest.json \
  --no-write --verify-existing

./build/mm_benchmark --events 1000000 --runs 5 --warmup 100000

cd python
uv sync --frozen --all-groups
uv run ruff check src tests
uv run ruff format --check src tests
uv run mypy src
LOBFORGE_REPLAY_CLI=../build/lobforge_replay uv run pytest -q
```

The exact ten-module coverage command, sanitizer flags and separate fuzz commands are pinned in
`.github/workflows/ci.yml`; the public claims index records the scope and limitations of each
headline result.
