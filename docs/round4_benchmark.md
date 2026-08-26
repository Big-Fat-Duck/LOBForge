# Round 4 benchmark methodology and results

## Environment and method

The full Round 4 load ran on an AMD Ryzen 7 5800H (16 logical CPUs), WSL2 Linux
`6.6.87.2-microsoft-standard-WSL2`, 15,708,380 KiB available memory, GCC 11.4.0 and CMake Release
flags `-O3 -DNDEBUG`. The benchmark pre-initializes a valid two-sided factual book, warms 100,000
events, then performs five independent 1,000,000-event runs.

Each timed event follows the production ordering `before_market -> FactualState::apply ->
after_market`. A symmetric strategy maintains active bid/ask shadow quotes, so the measurement
includes factual application, scheduler draining, causal snapshots, strategy decisions, lifecycle
checks and audit state. Input generation, warm-up and percentile selection are outside the reported
throughput interval. Per-event `steady_clock` samples produce p99; `/proc/self/status` `VmHWM`
provides peak RSS.

## Round 4 result

| Measurement | Five runs | Median | Gate | Status |
|---|---|---:|---:|---|
| Facts/second | 1,206,313; 1,206,019; 1,183,927; 1,284,725; 1,326,146 | 1,206,313/s | >=500,000/s | PASS |
| p99 processing latency | 2.9; 2.9; 2.9; 2.9; 2.9 us | 2.9 us | <20 us | PASS |
| Peak RSS | one process high-water mark | 74,252,288 B | <=1.5 GiB | PASS |

The observed throughput is 2.41 times the mandatory floor; p99 is 14.5% of the limit; RSS is 4.61% of
the limit.

## Earlier-round regression

Round 1 and Round 2 were rebuilt with Windows GCC 13.1.0 Release on the same AMD Ryzen 7 5800H host.
Each benchmark used its existing pre-generated 1,000,000-record workload and 100,000-record warm-up.
The registered pre-Round-3 baselines, rather than the best later observation, define the -10% gate.

| Benchmark | Five facts/commands per second runs | Median | Registered baseline | Change | Gate |
|---|---|---:|---:|---:|---|
| Round 1 `mixed` | 4,307,479; 2,943,360; 4,852,406; 4,533,593; 4,464,451 | 4,464,451 | 4,893,861 | -8.78% | PASS |
| Round 2 `decoder_apply` | 4,646,682; 4,389,940; 4,571,238; 4,460,472; 4,544,353 | 4,544,353 | 4,760,340 | -4.54% | PASS |

The low second Round 1 sample remains visible; the preregistered median, not a selected run, is the
decision statistic. No Round 1/2 benchmark source or core implementation was changed in Round 4.
