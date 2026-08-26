# Round 1 engineering report

Date: 2026-08-25 (Asia/Singapore)

## Outcome

Round 1 is complete. All ten acceptance gates pass. The final fresh GCC Release build, the local
Clang Release build, all 27 named correctness tests, 100,000 differential randomized commands,
ASan/UBSan/LSan validation, formatting, four benchmark workloads, and the mandatory mixed-workload
performance thresholds passed. No repository existed at the start, so every source-controlled file
listed below was newly created; no unrelated user work was rewritten.

## Change summary

The implementation adds a dependency-free C++20 matching library with typed commands/events,
price-time priority, GTC/IOC/FOK/PostOnly and market behavior, direct indexed cancel/reduce/replace,
atomic validation of priority-losing replacement, overflow-safe aggregates, deterministic inspection,
and a debug/test invariant checker. It also adds an independent scan-based reference model, a named
deterministic and randomized suite, a fixed-seed four-workload benchmark, GCC/Clang/sanitizer CI,
formatting configuration, public documentation, and this evidence report.

## Files added

- `.clang-format`
- `.gitignore`
- `.github/workflows/ci.yml`
- `CMakeLists.txt`
- `README.md`
- `include/lob/types.hpp`
- `include/lob/commands.hpp`
- `include/lob/events.hpp`
- `include/lob/order_book.hpp`
- `src/order_book.cpp`
- `tests/reference_order_book.hpp`
- `tests/reference_order_book.cpp`
- `tests/order_book_tests.cpp`
- `benchmarks/order_book_benchmark.cpp`
- `docs/adr/0001-matching-engine-semantics.md`
- `docs/round1_report.md`

Generated `build*` directories and the exploratory root test executable are ignored and are not
project deliverables. The directory was not a Git repository, so there is no commit/status baseline.

## Validation commands and results

The Windows shell first prepended the CLion-bundled CMake 3.29.6, Ninja 1.12.0, MinGW GCC 13.1.0,
and clang-format directories to `PATH`. The clean final Release validation was then exactly:

```powershell
cmake -S . -B build-release-final -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-final
ctest --test-dir build-release-final --output-on-failure
cmake --build build-release-final --target format-check
.\build-release-final\order_book_tests.exe
```

Result: configure/build succeeded from a previously absent directory, CTest passed `1/1` in 0.24 s,
the formatting target passed, and the executable reported `27/27 tests passed;
randomized_commands=100000 seeds=20`.

The local Clang 14 Release validation in Ubuntu 22.04 WSL2 was:

```sh
cmake -S . -B build-clang-final -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build-clang-final -j2
ctest --test-dir build-clang-final --output-on-failure
./build-clang-final/order_book_benchmark --smoke
```

Result: build and CTest passed (`1/1`, 0.12 s); all four 10,000-command smoke workloads completed.
The smoke checksums matched the Windows GCC build exactly, providing an additional cross-compiler,
cross-OS deterministic-state check.

The final sanitizer validation in Ubuntu 22.04 WSL2 was:

```sh
cmake -S . -B build-sanitize-wsl -DCMAKE_BUILD_TYPE=Debug \
  -DLOB_ENABLE_SANITIZERS=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-sanitize-wsl -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize-wsl --output-on-failure
```

Result: Clang 14 built all targets; CTest passed `1/1` in 7.60 s, representing all 27 internal tests
and 100,000 randomized commands. AddressSanitizer, UndefinedBehaviorSanitizer, and ASan leak
detection emitted zero findings. WSL lacked Ninja, so this local run used CMake Unix Makefiles; CI
installs Ninja and runs the same compiler/sanitizer flags with it.

For full disclosure, a prior Windows MinGW sanitizer attempt configured and compiled but could not
link because that bundled toolchain does not ship sanitizer runtimes. Its exact failing configuration
and build were:

```powershell
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLOB_ENABLE_SANITIZERS=ON \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build-sanitize
```

For that historical attempt, `g++` resolved to the GCC 13.1 executable bundled with the local
CLion installation. The repository does not depend on that installation path.

The linker evidence was `cannot find -lasan` and `cannot find -lubsan`. This is not an acceptance
blocker because the supported Clang/Linux ASan+UBSan+LSan run subsequently passed on the final
source.

The mandatory measured benchmark command was:

```powershell
.\build\order_book_benchmark.exe
```

The executable was the final-source GCC 13.1 CMake Release target (`-O3 -DNDEBUG`). It generated all
streams before timing with `std::mt19937_64` seed `12648430`, warmed 100,000 commands on a separate
book, processed 1,000,000 commands per workload for throughput, then replayed on a fresh book for
per-command `std::chrono::steady_clock` latency. Generation, percentile sorting, invariant checks,
checksum calculation, and output were outside timed regions. Both timed replays had to produce the
same canonical snapshot.

## Test coverage

There are 27 named tests: the 25 required semantic/boundary groups, an explicit same-price/smaller
replacement-priority case, and the differential randomized case. Deterministic tests cover price and
FIFO priority, maker/taker partial fills, multi-level sweeps, maker-price execution, all TIF paths,
market expiry, cancellation, both priority-retaining mutation paths, both priority-losing replacement
paths, crossing replacement, replacement rollback, typed invalid/unknown/duplicate cases, empty-book
inspection, sequence ordering, ten-run canonical determinism, and `UINT64_MAX` aggregate boundaries.

The differential test executes 5,000 commands for each of these 20 seeds, for 100,000 commands total:

```text
1, 7, 19, 42, 73, 101, 313, 997, 2003, 4099,
8191, 16381, 32749, 65521, 131071, 262139, 524287, 1000003, 4294967, 9999991
```

Streams include valid and invalid adds, limit/market crossing, all TIFs, cancels, reductions,
replacements, duplicates, and unknown IDs. After every command, production and reference compare the
entire ordered event vector (therefore trades and rejection enums), best bid/ask, full aggregate depth,
active IDs/side/price/remaining quantity/priority/queue position, and production invariants. Failure
output includes seed, zero-based command index, the last 12-command reproduction context, invariant
error, and production canonical snapshot.

The ten-run determinism case compares the complete ordered event stream and canonical snapshot after
every command across ten newly constructed engines. The Clang/GCC smoke benchmark checksum equality
adds independent cross-platform evidence.

## Benchmark results

Environment reported by the benchmark:

- Compiler: GCC 13.1.0
- Optimization: `-O3 -DNDEBUG`
- CPU: AMD Ryzen 7 5800H with Radeon Graphics
- Logical cores: 16
- OS: Windows
- Virtualization detection: `yes` (CPUID hypervisor bit; local Windows host with WSL2/virtualization)
- Clock: `std::chrono::steady_clock`, reported in nanoseconds

The hypervisor flag was disclosed but did not prevent meaningful monotonic local measurements: all
workloads completed normally, latency resolution was 100 ns, replay checksums/invariants passed, and
the mixed result had substantial margin over both required thresholds.

| Workload | Commands | Commands/s | p50 ns | p95 ns | p99 ns | Checksum |
|---|---:|---:|---:|---:|---:|---:|
| add_cancel | 1,000,000 | 7,047,618 | 200 | 200 | 300 | 3718438186451411385 |
| crossing | 1,000,000 | 4,944,126 | 200 | 400 | 500 | 2912460701802407433 |
| mixed | 1,000,000 | 3,952,356 | 200 | 700 | 1,200 | 324670585235293660 |
| deep_book | 1,000,000 | 6,718,082 | 200 | 300 | 500 | 12944147475874934866 |

Performance gate: **PASS**. Mixed throughput was 3.95 times the 1,000,000 commands/s minimum and
mixed p99 was 1.2 microseconds, below the 10-microsecond maximum.

## Acceptance gates

| ID | Result | Evidence |
|---|---|---|
| A1 Clean build | **PASS** | Fresh `build-release-final` Ninja/GCC 13.1 configure and seven-step build succeeded; CTest passed. |
| A2 Deterministic behavior | **PASS** | Named test compared complete events and every canonical snapshot across 10 runs; GCC/Windows and Clang/Linux smoke checksums also matched. |
| A3 Matching semantics | **PASS** | All required deterministic cases are represented in the 27/27 passing named suite. |
| A4 Invariants | **PASS** | Debug processing asserts invariants; tests explicitly check after every deterministic and each of 100,000 randomized commands. |
| A5 Differential testing | **PASS** | Production and independent scan oracle agreed after 5,000 commands × 20 fixed seeds = 100,000. |
| A6 Sanitizers | **PASS** | Final-source Clang 14 Debug suite passed under ASan+UBSan with leak detection, zero findings, 7.60 s. |
| A7 Performance | **PASS** | Mixed: 3,952,356 commands/s and 1,200 ns p99 on disclosed Release host; required ≥1,000,000 and <10,000 ns. |
| A8 CI | **PASS** | `.github/workflows/ci.yml` defines pinned-checkout GCC Release, Clang Release, Clang ASan/UBSan/LSan, format, and threshold-free smoke jobs. Corresponding local GCC, Clang, sanitizer, format, test, and smoke commands passed. |
| A9 Documentation | **PASS** | README contains purpose/scope, architecture, exact semantics, integer prices, commands, complexity, benchmark method, non-goals, and required example; ADR and this evidence report are present. |
| A10 Scope and privacy | **PASS** | No ITCH/strategy/network/persistence implementation, credentials, market data, or live endpoints were added; no publish, commit, push, PR, or release action occurred. |

Overall acceptance result: **PASS (A1–A10)**. There are no unresolved Round 1 blockers.

## Remaining technical debt and Round 2 recommendations

- The standard-library design intentionally favors auditability. Measure before considering custom
  allocators, flat hash indexes, intrusive queues, or price ladders; preserve the reference oracle.
- Hash-index operations have standard expected-constant rather than hard real-time worst-case bounds.
- Logical validation failures are atomic, but `std::bad_alloc` during a priority-losing replacement
  does not carry a bespoke recovery guarantee. A later fault-injection/allocation policy can address
  this if the deployment model requires it.
- The 64-bit event and priority counters fail fast rather than wrap at their unreachable practical
  limit. A durable multi-session sequence policy belongs with future persistence/recovery design.
- Round 2 should keep protocol parsing outside the core, convert feed units explicitly to ticks, add
  captured-license-safe synthetic fixtures, and preserve deterministic command/event replay before
  adding strategy or concurrency layers.
