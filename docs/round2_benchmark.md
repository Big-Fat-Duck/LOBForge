# Round 2 benchmark methodology and results

## Method

`itch_replay_benchmark` generates all payloads and workloads before timing and consumes preloaded
memory, so storage is outside the gate. Release uses `-O3 -DNDEBUG`. Each workload warms 100,000
records and measures 1,000,000. Throughput encloses only sequential decode/apply work. A separate
fresh replay samples every record with `std::chrono::steady_clock`; reported p50/p95/p99 are sorted
nanoseconds. Generation, warm-up, sorting, checksums, and output are outside the timed interval.

Workloads are deterministic:

- `decoder_only`: uniform rotation through all 23 literal valid application-message types;
- `decoder_apply`: 20% each add, execute, cancel, replace, and delete;
- `deep_multi_symbol`: 64 symbols and 3,200 levels, then 50% delete/50% add;
- `permissive_error`: fully framed invalid records at a 0.1% rate.

## Validation environment

- Date: 2026-08-25
- CPU: AMD Ryzen 7 5800H with Radeon Graphics, 16 logical cores
- OS: Windows 10 build 19045.6466, virtualized environment reported by Round 1 benchmark
- Compiler: GCC 13.1.0
- Build: CMake Release, `-O3 -DNDEBUG`, C++20
- Clock: `std::chrono::steady_clock`

## Measured results

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

The required gates pass: decoder-only is 18.51 M/s (minimum 5 M/s), decoder+apply is 4.27 M/s
(minimum 2 M/s), and decoder+apply p99 is 500 ns (maximum strictly below 10,000 ns).

## Round 1 regression

The pre-Round-2 `mixed` median was 4,964,210 commands/s from 4,798,252, 4,984,741, and 4,964,210.
The post-Round-2 median is 5,031,864 commands/s from 5,033,510, 5,031,864, and 4,904,473.
That is a 1.36% increase, so the no-more-than-10% regression gate passes. Round 1 source and benchmark
semantics were not changed.
