# mdbus — a low-latency market-data bus

A lock-free, single-producer / multi-consumer market-data distribution bus that fans a
synthetic exchange feed out to N cores, with rigorously measured tail latency — and a
characterisation of *what* determines that tail (load, fan-out, false sharing, memory
ordering, and OS jitter).

> **Status: in progress.** This repo is being built step by step. See
> [`DESIGN.md`](DESIGN.md) for the full design, and the roadmap below for where it is.

## Why

A trading system consumes one market-data feed and must distribute every tick to many
strategy threads with minimal, *predictable* latency. The hard part is not throughput —
it is the **tail** (p99.9), especially under bursts.

This project grows out of a cross-core ping-pong latency harness (preserved in
[`legacy/pingpong.cpp`](legacy/pingpong.cpp)), the toy used in Jane Street's "controlling
jitter" talk to stand in for market-data ticks. The bus is the real thing that toy
represents, and the experiments probe where the clean 2-core abstraction **leaks** once
there is real load, real fan-out, and real message sizes.

## Roadmap

| Step | Component | Status |
|------|-----------|--------|
| S0 | Repo scaffold, CMake, layout | ✅ done |
| S1 | `timing.hpp` — rdtsc, invariant-TSC check, calibration | ✅ done |
| S2 | `message.hpp`, `affinity.hpp` | ✅ done |
| S3 | `spsc_ring.hpp` + tests + TSan | ✅ done |
| S4 | `stats.hpp`, CSV/meta output, `plots/plot_run.py` | 🔨 stats + output done, plots next |
| S5 | `spmc_ring.hpp` — broadcast + gap detection | ⬜ |
| S6 | `source.hpp` — constant / Poisson / Hawkes generator | ⬜ |
| S7 | `bench/bench_main.cpp` — config-driven runner | ⬜ |
| S8 | mutex + condvar baseline comparator | ⬜ |
| S9 | Experiments E1–E8, `run_all.sh`, comparison plots | ⬜ |
| S10 | Findings write-up | ⬜ |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`ctest` currently runs the timing self-test, which validates the measurement
instrument before anything is measured with it: invariant TSC confirmed via
CPUID, TSC frequency calibrated against `CLOCK_MONOTONIC`, and the two clocks
cross-checked over a 50 ms window. On this machine calibration reproduces to
within ~5 ppm across runs and agrees with the kernel's boot-time estimate
(2918.400 MHz) to ~13 ppm.

Measured instrument overhead, needed to interpret every later figure:

| Read | Cost |
|------|------|
| `rdtsc_ordered` (rdtscp + lfence) | ~12–13 ns |
| `rdtsc_relaxed` (bare rdtsc) | ~6–7 ns |
| `std::chrono::steady_clock::now()` | ~20–25 ns |

## Core placement

Runs are on bare-metal Arch Linux, i7-13700H — a hybrid part: 6 P-cores (Raptor Cove,
SMT, 4.8–5.0 GHz, CPUs 0–11) and 8 E-cores (Gracemont, no SMT, 3.7 GHz, CPUs 12–19).

```
CPU:   0  1  |  2  3  |  4  5  |  6  7  |  8  9  | 10 11 | 12..19
CORE:  0     |  1     |  2     |  3     |  4     |  5    | 6..13
     4.8GHz    4.8      5.0      5.0      4.8      4.8     3.7 (E)
     └─SMT─┘  └─SMT─┘  └─SMT─┘  └─SMT─┘  └─SMT─┘  └SMT─┘   no SMT
```

Which core a thread lands on is a first-order effect, not a detail. The same
`rdtscp` + `lfence` sequence measures:

| Core | Ordered read | Relaxed read |
|------|--------------|--------------|
| cpu4 / cpu6 (P) | 10.96 ns | 5.48 ns |
| cpu12 (E) | 19.19 ns | 8.91 ns |
| cpu18 (E) | 18.50 ns | 10.96 ns |

**1.7× apart for identical instructions.** The frequency ratio (5.0/3.7 = 1.35×) only
explains part of it; the rest is microarchitecture. Left unpinned, the scheduler picks
an E-core for a short-lived benchmark process by default, so the default is the wrong
answer.

Two threads on SMT siblings of one physical core (e.g. cpu4 and cpu5) share L1 and L2,
so their "cross-core" latency never crosses a core. Benchmarks therefore pin to one CPU
per physical P-core, drawn from `{0, 2, 4, 6, 8, 10}`; cpu4/cpu6 are the default pair.

With six P-cores, a 1-producer/8-consumer fan-out does not fit on P-cores alone — that
configuration is reported separately and labelled.

**Not yet configured:** `isolcpus` / `nohz_full` are empty and SMT is on. Frequency
scaling is `intel_pstate` in active mode; note that `scaling_governor` reads `powersave`
while the effective control is the HWP energy-performance preference (`EPP=performance`).
These are phase-2 (E9) rungs.
