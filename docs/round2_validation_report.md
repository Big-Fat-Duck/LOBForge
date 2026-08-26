# Round 2 validation report

- Validation date: 2026-08-25 (Asia/Singapore)
- Normative baseline: Nasdaq TotalView-ITCH 5.0, June 11, 2026
- Host: Windows 10 build 19045.6466, AMD Ryzen 7 5800H, 16 logical cores
- GCC: 13.1.0 MinGW, CMake 3.29.6, Ninja, Release `-O3 -DNDEBUG`
- Clang: 14.0.0, Ubuntu 22.04 WSL1, CMake 3.22.1, Unix Makefiles

The installed WSL2 VM could not start because the host HCS service returned `0x800705aa` and the
non-administrator validation process could not restart that service. Validation therefore used an
isolated, SHA-256-verified official Ubuntu 22.04 rootfs imported as a temporary WSL1 distribution.
This still uses the documented Linux Clang sanitizer/libFuzzer runtimes. The project gained no
runtime or source dependency from this validation environment.

## Source and dependency audit

Production code uses only the C++20 standard library. No package manager, runtime library,
framework, vendored code, or third-party header was added. Python's standard-library `json` module
is used only by an optional CTest output validator. CMake, Ninja/Make, GCC, Clang, clang-format, and
sanitizer runtimes are build/validation tools, not shipped dependencies. Removing the Round 2
feature requires only removing its targets and source tree; there is no dependency lock-in.

The supplied workspace has no `.git` directory, so `git status` and `git diff` reported “not a git
repository”. Files were instead enumerated and reviewed directly. No remote, push, commit, release,
package upload, credential, restricted dataset, or deployment was created.

## Exact validation commands

Windows GCC Release (the CLion tool directories were prepended to `PATH`):

```powershell
cmake -S . -B build-r2-final3-gcc -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build-r2-final3-gcc -j 2
ctest --test-dir build-r2-final3-gcc --output-on-failure
.\build-r2-final3-gcc\itch_replay_tests.exe
```

Clang Release in the isolated Ubuntu environment:

```sh
cmake -S . -B build-r2-clang-release \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build-r2-clang-release -j2
ctest --test-dir build-r2-clang-release --output-on-failure
```

Both final Release runs produced this CTest result:

```text
100% tests passed, 0 tests failed out of 9
```

The Round 2 executable reported:

```text
17/17 Round 2 tests passed; golden_types=23 randomized_mutations=100000 seeds=20
```

The unchanged Round 1 executable reported:

```text
27/27 tests passed; randomized_commands=100000 seeds=20
```

Sanitizers and leak detection:

```sh
cmake -S . -B build-r2-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_SANITIZERS=ON \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build-r2-sanitize -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-r2-sanitize --output-on-failure
```

Final result:

```text
100% tests passed, 0 tests failed out of 9
Total Test time (real) = 242.08 sec
```

There were zero AddressSanitizer, UndefinedBehaviorSanitizer, or LeakSanitizer findings.

Fuzz build and final fresh-corpus campaign:

```sh
cmake -S . -B build-r2-fuzz \
  -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_SANITIZERS=ON \
  -DLOB_BUILD_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-r2-fuzz -j2
./build-r2-fuzz/itch_replay_tests \
  --write-fuzz-corpus build-r2-fuzz/final-corpus
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build-r2-fuzz/itch_fuzz build-r2-fuzz/final-corpus \
  -max_total_time=30 -timeout=5 -print_final_stats=1 -verbosity=0
```

Unedited final statistics:

```text
stat::number_of_executed_units: 287685
stat::average_exec_per_sec:     9280
stat::new_units_added:          381
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              427
```

The run consumed 31 wall-clock seconds after loading 27 golden/malformed seeds, returned zero, and
reported no crash, timeout, ASan, UBSan, or LSan finding.

Formatting:

```sh
cmake --build build-r2-clang-release --target format-check
```

```text
[100%] Checking C++ formatting
[100%] Built target format-check
```

## Determinism and fixture evidence

The checked-in hex fixture generates 810 identical bytes under both toolchains. SHA-256 (used only
to prove the fixture files are identical) is
`2241d6a2b82afa09a5b9936739365fe5994fa1f725780debda886a8ea279acbd`. Ten consecutive GCC runs and
ten consecutive Clang runs all produced:

```text
state_digest_fnv1a64=eaa0ddd8309c94c0
```

Its exact golden expectations are 28 records, all 23 types, six lifecycle `S` messages, one symbol,
one active order, one price level, four ledger records, add/cancel/execute volumes 380/245/55,
printable/non-printable/broken volumes 1075/30/50, and AAPL bid 80 shares at Price(4) 1,234,400.

## Performance evidence

Command:

```powershell
.\build-r2-final3-gcc\itch_replay_benchmark.exe
```

Unedited measured output:

```text
lobforge_itch_benchmark_v1
compiler=GCC 13.1.0
optimization_flags= -O3 -DNDEBUG
cpu_model=AMD Ryzen 7 5800H with Radeon Graphics
logical_cores=16
operating_system=Windows
clock=std::chrono::steady_clock input=preloaded_memory warmup_records=100000 sample_records=1000000
workload records_per_second p50_ns p95_ns p99_ns checksum distribution
decoder_only 18509092 100 100 100 1108152157524130454 uniform_23_types
decoder_apply 4272890 200 400 500 12247439309150447187 add20_execute20_cancel20_replace20_delete20
deep_multi_symbol 3999796 300 400 600 2884898149770772806 symbols64_levels3200_delete50_add50
permissive_error 7816035 0 0 0 15134547118015425890 invalid_rate_0.1_percent
```

All three Round 2 gates pass. Round 1 `mixed` pre-change median was 4,964,210 commands/s; the final
three values were 5,033,510, 5,031,864, and 4,904,473, giving median 5,031,864 and a 1.36% increase.
The allowed floor was 4,467,789 (90% of baseline).

## Acceptance gates

| Gate | Result | Evidence |
|---|---|---|
| B1 | PASS | Fresh GCC 13.1 and Clang 14 C++20 Release builds; both 9/9 CTest; Round 1 27/27 |
| B2 | PASS | 23 typed, field-complete messages and every coverage-matrix row `Complete` |
| B3 | PASS | Explicit BE 16/32/48/64 reader; literal golden fields; all truncated prefixes, wrong lengths, enums, unknown type, and framing errors |
| B4 | PASS | Separate factual state, required mutation set, ledger/reference state, typed semantic errors, invariant checks |
| B5 | PASS | 28-record all-type fixture matches exact state, statistics, top-N, ledger, and digest |
| B6 | PASS | Independent vector/scan oracle: 20 fixed seeds and 100,000 valid mutations; invalid atomicity tests |
| B7 | PASS | Strict/permissive, stable diagnostics, max-message slice, deterministic text/JSON, valid JSON, and exit 0/2/3/4 tests; invariant exit 5 documented and guarded |
| B8 | PASS | Same 810-byte fixture and `eaa0ddd8309c94c0` over 10 GCC plus 10 Clang runs |
| B9 | PASS | Final ASan/UBSan/LSan 9/9; libFuzzer 31 s and 287,685 executions; zero findings |
| B10 | PASS | 18.51 M/s decode, 4.27 M/s decode+apply, 500 ns apply p99; Round 1 median +1.36% |
| B11 | PASS | GCC/Clang CI matrix, sanitizer job, 30 s fuzz job, format job, benchmark smoke; local CI-equivalent commands pass |
| B12 | PASS | README, coverage matrix, two ADRs, CLI schema/exits, benchmark report, validation report, limitations, dependency audit |
| B13 | PASS | Offline/private scope preserved; no transport, strategy, binding, deployment, credentials, restricted data, or publication |

## Real-data evidence

`BLOCKED: DATASET_NOT_PROVIDED`

The workspace and user-supplied document area contained no legally identified official sample or
user-supplied TotalView-ITCH capture. Build artifacts and the checked-in synthetic fixture were
excluded from consideration. No market data was downloaded or committed to change this status.
Synthetic results are engineering evidence only; they do not establish production readiness or
profitability.
