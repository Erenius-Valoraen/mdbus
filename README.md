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
| S2 | `message.hpp`, `affinity.hpp` | ⬜ |
| S3 | `spsc_ring.hpp` + tests + TSan | ⬜ |
| S4 | `stats.hpp`, CSV/meta output, `plots/plot_run.py` | ⬜ |
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

## Measurement caveat

Development happens under WSL2 on an i7-13700H. WSL presents a flattened, synthetic CPU
topology (a hybrid 6 P-core + 8 E-core part appears as 10 uniform cores × 2 threads, with
no `MAXMHZ` reported), so core pinning cannot distinguish a P-core from an E-core and
absolute latencies are not trustworthy. **Methodology is developed here; headline numbers
require bare metal** with `isolcpus` / `nohz_full`. Every published figure states which
machine produced it.
