# Low-Latency Market-Data Bus — Design Doc

**Working name:** `mdbus`
**Status:** written before implementation. The SPSC ring, timing layer, paced benchmark and runtime core isolation are built; the SPMC broadcast ring and the load generator are not.
**Language:** C++ (17 or 20) · **Platform:** Linux (bare metal for final numbers; WSL acceptable for dev)

---

## 1. One-line thesis

> A lock-free, single-producer/multi-consumer market-data distribution bus that fans a synthetic exchange-rate feed out to N cores, with rigorously measured tail latency — and a characterization of *what* determines that tail (load, fan-out, false sharing, memory ordering, and OS jitter).

This project grows out of a cross-core SPSC ping-pong latency harness. The ping-pong is the *atom* of a market-data bus (as framed in Jane Street's "controlling jitter" talk, where the toy stands in for exchange market-data ticks). This project builds the real thing the toy represents and then investigates where the clean 2-core ping-pong abstraction **leaks** once you have real load, real fan-out, and real message sizes.

---

## 2. Motivation & framing

A trading system consumes one market-data feed and must distribute every tick to many strategy threads with minimal, *predictable* latency. Jitter that delays a tick by even microseconds means trading on stale data. The hard part is not throughput — it is the **tail** (p99.9), especially under bursts.

The ping-pong toy hides five things that this project deliberately surfaces (these become the findings / writeup sections):

1. **Closed loop → bursty open loop.** One message in flight never builds a backlog; real feeds microburst.
2. **Latency-under-load correlation.** Does p99.9 blow out *exactly during bursts* (when it hurts most)?
3. **Coordinated omission.** With externally-paced arrivals, latency must be measured from a message's *intended* arrival time, not from when the consumer got to it.
4. **Fan-out coherence.** 1→N introduces a read-shared "published sequence" hotspot and slowest-consumer gating that a 2-core model cannot have.
5. **Message size.** One `int` fits a register; a realistic L2 update spans cache lines, changing coherence cost.

---

## 3. Goals & non-goals

### Goals
- A correct, fast, **lock-free SPMC broadcast ring** (LMAX Disruptor pattern).
- A **synthetic load generator** with controllable burstiness (Hawkes process) targeting ~1 GB/s peak (loose convention — configurable).
- A **measurement harness** with `rdtsc` timing, coordinated-omission-correct latency, and full tail statistics.
- A suite of **experiments** producing publication-quality plots that answer the five questions above.
- **Reproducibility:** one command builds, runs, and regenerates every plot. No external data, no network, no accounts.

### Non-goals
- Not a trading system, strategy engine, or backtester. Consumers do trivial synthetic work.
- Not real market data (explicitly out of scope — see §7 for why, and phase-2 hook).
- Not cross-machine / networked. Single box, multiple cores.
- Not a general-purpose queue library; it is a benchmark + study.

---

## 4. Headline deliverable

A README leading with a single defensible sentence and its supporting plots, e.g.:

> "A lock-free market-data bus that fans a synthetic feed out to N cores. On bare-metal Linux with core isolation, p99.9 tick-to-consumer latency is **X µs** at ~1 GB/s; here is each optimization's contribution to that tail, and here is how the tail degrades under microbursts and with consumer count."

---

## 5. Architecture

```
                 intended_tsc schedule (pre-generated, Hawkes)
                              │
                              ▼
   ┌───────────┐      ┌──────────────┐        ┌──────────────────────────┐
   │  Load     │─────▶│  Producer     │──────▶ │  SPMC broadcast ring      │
   │  Generator│      │  (1 core)     │  publish│  (power-of-2 slots)       │
   └───────────┘      └──────────────┘        └──────────────────────────┘
                                                  │        │        │
                                            acquire│  acquire│  acquire│  (every consumer reads every msg)
                                                  ▼        ▼        ▼
                                             ┌───────┐┌───────┐┌───────┐
                                             │Cons 0 ││Cons 1 ││Cons k │  (each pinned to its own core)
                                             └───┬───┘└───┬───┘└───┬───┘
                                                 └────────┴────────┘
                                                          ▼
                                            per-consumer latency samples
                                                          ▼
                                              CSV/binary  →  Python plots
```

- **1 producer**, pinned to one core, publishing to a single broadcast ring.
- **k consumers**, each pinned to its own core, each reading *every* message (broadcast semantics).
- Latency is measured per consumer as `receive_tsc − intended_tsc`.

---

## 6. Key design decisions (all approved)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| D1 | Build order | SPSC ring first, then SPMC broadcast | Each stage independently testable; SPSC is the correctness foundation |
| D2 | Ring capacity | Bounded, **power of 2**, index via `seq & (N-1)` | Avoids modulo divide on the hot path |
| D3 | Slot layout | Fixed-size message; **seq stored in slot**; `alignas(64)` | Seq-in-slot is the publish flag + overwrite detector |
| D4 | Cursor layout | Producer cursor and each consumer cursor on **separate cache lines** (padded), with a **compile-time toggle** to disable padding | False sharing is *the* silent latency killer; the toggle turns it into a measured finding |
| D5 | Slow-consumer policy | **Overwrite + gap detection** (non-blocking producer); optional **gating** mode | Market-data-realistic (feeds drop); lets us study drops under load. Gating offered as a comparison mode |
| D6 | Memory ordering | `release` publish / `acquire` read (minimal correct); **compile flag for `seq_cst`** | Makes the fence cost a measured experiment |
| D7 | Timing | `rdtsc` + calibration (measure TSC freq, check invariant TSC); `steady_clock` fallback | Lowest-overhead timer; calibration is part of the rigor |
| D8 | Latency definition | `receive_tsc − intended_tsc` (coordinated-omission correct) | Intended time is externally fixed, so producer stalls can't hide latency |
| D9 | Load model | **Hawkes process** (self-exciting) for bursts; Poisson + constant modes for controlled sweeps | Hawkes reproduces microbursts and, crucially, lets us *sweep* burst intensity |
| D10 | Data | **Synthetic only**, no real feeds | Fully reproducible / GitHub-able; real data is optional phase 2 |
| D11 | Plotting | C++ writes CSV/binary; **Python + matplotlib** plots | Reuse existing pipeline; keep C++ hot path clean |
| D12 | Build | **CMake**, `-O3 -march=native -pthread`; one-command runner | Reads as a real project; reproducibility |

---

## 7. Why synthetic (and the phase-2 hook)

Real free crypto tick data (e.g. Tardis first-of-month Binance, 100 ns arrival stamps) is only ~hundreds–thousands of msgs/s — it cannot saturate the bus, and it is one fixed trace that cannot be *swept*. The synthetic Hawkes generator is therefore the **primary instrument**: it alone can dial load and burstiness to trace the latency-vs-load curve. Real data would add complexity and a data dependency without enabling the core experiments.

**Phase-2 hook (out of scope now):** a `ReplaySource` interface (§8.4) mirrors the generator's output type, so a recorded-data replayer (Tardis crypto, or Databento CME MBO with true nanosecond timestamps) can be dropped in later as a realism anchor without touching the ring or harness.

---

## 8. Component specifications

### 8.1 Message format

```cpp
// Default 64 B (one cache line). A 128 B variant exists for the
// "multi-cache-line message" experiment (D-size sweep).
struct alignas(64) Message {
    uint64_t seq;          // producer-assigned sequence number  (ALSO the publish flag)
    uint64_t intended_tsc; // scheduled send time — reference for coordinated-omission latency
    uint64_t send_tsc;     // actual send time (producer-side slack analysis)
    int64_t  price;        // fixed-point (e.g. 1e-8 ticks)
    uint64_t size;         // quantity
    uint32_t symbol_id;
    uint8_t  side;         // 0 = bid, 1 = ask
    uint8_t  level;        // book level
    uint16_t flags;        // update type: add / modify / cancel / trade
    // implicit padding to 64 B
};
```
`seq` MUST be written **last** by the producer (with release), because consumers use it as the "slot is ready" flag.

### 8.2 SPSC ring (Phase 0 foundation)

Purpose: the correctness/perf foundation and a baseline number. Single producer, single consumer, FIFO, no drops.

```cpp
template <typename T, size_t N>   // N is power of 2
class SpscRing {
    static_assert((N & (N-1)) == 0, "N must be power of two");
    alignas(64) std::atomic<uint64_t> head_{0};   // producer writes  (own cache line)
    alignas(64) std::atomic<uint64_t> tail_{0};   // consumer writes  (own cache line)
    alignas(64) std::array<T, N> buf_;
public:
    bool try_push(const T& m);   // false if full  (head - tail == N)
    bool try_pop(T& out);        // false if empty (head == tail)
};
```
- Producer: load `tail_` (acquire) to check space, write `buf_[head & (N-1)]`, store `head_+1` (release).
- Consumer: load `head_` (acquire), read slot, store `tail_+1` (release).
- `head_`/`tail_` on separate cache lines (D4). Padding toggle via a template/config flag.

### 8.3 SPMC broadcast ring (core of the project)

Purpose: one producer, k consumers, **every consumer receives every message**. Non-blocking producer with overwrite; consumers detect being lapped.

Layout:
```cpp
struct alignas(64) Slot { Message msg; };            // seq lives inside msg
alignas(64) std::atomic<uint64_t> published_{0};     // last fully-published seq (release/acquire)
alignas(64) std::array<Slot, N> buf_;                // N power of 2
// each consumer owns:  alignas(64) uint64_t cursor;  // next seq it wants (separate cache lines)
```

**Producer publish (never blocks in overwrite mode):**
```
s = next_seq++
slot = buf_[s & (N-1)]
slot.msg = payload           // write body
slot.msg.seq = s             // write seq LAST
published_.store(s, release) // announce
```

**Consumer read of its cursor c (overwrite-aware protocol):**
```
loop:
  p = published_.load(acquire)
  if c > p: spin/backoff; continue          // nothing new yet
  slot = buf_[c & (N-1)]
  s = slot.msg.seq (acquire)
  if s == c:
      copy payload
      if slot.msg.seq (re-read) != c: GAP   // producer overwrote mid-read → dropped
      else: process; c++
  else if s > c:                            // producer already lapped us
      GAP: dropped = s - c (approx); c = (p - N + 1) or resync target; count drops
```
- **Gap detection** is correctness-critical and the #1 thing to unit-test (see §11).
- **Overwrite safety window:** with capacity N, a consumer is safe as long as it stays within N of `published_`. Falling >N behind = guaranteed drop (realistic).
- **Optional gating mode (D5):** producer, before overwriting slot for seq `s`, waits until `min(all consumer cursors) > s - N`. Selected at compile time or runtime flag; used for a "never-drop vs never-block" comparison.

### 8.4 Load generator

Two responsibilities: (a) decide *when* each message is sent (schedule), (b) fill payloads.

- **Arrival models** (selectable):
  - `constant(rate)` — fixed inter-arrival; clean baseline for sweeps.
  - `poisson(rate)` — memoryless.
  - `hawkes(mu, alpha, beta)` — self-exciting; `λ(t) = μ + Σ_{t_i<t} α·e^{−β(t−t_i)}`. Produces clustered microbursts. `mu` scales mean rate; `alpha/beta` control burst intensity/decay.
- **Schedule generation:** pre-generate an array of `intended_tsc` timestamps (offline, before timing starts) via Ogata thinning for Hawkes. This keeps the hot path free of RNG/exp() and provides the fixed `intended_tsc` reference for coordinated omission (D8).
- **Two run modes:**
  - **Paced** (for latency): producer busy-waits until `rdtsc() >= intended_tsc[i]` before publishing message i. If it falls behind schedule, it publishes late — and the late-ness is captured because latency is measured against `intended_tsc`, not actual send. This is the coordinated-omission-correct design.
  - **Saturation** (for max throughput): ignore pacing, publish as fast as possible; report achieved msgs/s and GB/s.
- **Throughput target:** `~1 GB/s` is a configurable knob, not a hard requirement. At 64 B/msg that is ~16 M msg/s; document achieved rate per run. Treat 1 GB/s as an aspirational upper end of the sweep.

Interface (so a phase-2 replayer can substitute):
```cpp
struct Event { uint64_t intended_tsc; Message payload; };
class Source {                       // implemented by Generator now, Replayer later
    virtual bool next(Event& out) = 0;   // false at end of stream
};
```

### 8.5 Measurement harness

- **`rdtsc` wrapper** with:
  - startup **calibration**: measure TSC ticks per nanosecond against `clock_gettime(CLOCK_MONOTONIC)` over a short interval.
  - **invariant-TSC check** via CPUID; warn (don't crash) if absent.
  - serializing read where needed (`rdtscp` / `lfence; rdtsc`) — document the choice.
- **Per-consumer latency:** on each processed message, record `receive_tsc − intended_tsc` (converted to ns at report time).
- **Recording:** pre-sized `std::vector<uint64_t>` per consumer (no allocation/IO in the hot loop). Optionally HdrHistogram for very long runs (phase 2).
- **Warmup:** discard the first configurable fraction (default 10%) before stats.
- **Reported stats per run:** count, mean, p50, p90, p99, p99.9, p99.99, max, achieved msgs/s and GB/s, drop count/rate per consumer.

### 8.6 Core placement

- `pin_to_core(int)` via `pthread_setaffinity_np` (already prototyped).
- Config: producer core, list of consumer cores. **Check affinity return codes** and verify against `sched_getcpu()`.
- Bare-metal recommendation documented in README: kernel cmdline `isolcpus=… nohz_full=… rcu_nocbs=…`, plus `taskset`. (Applying these is a phase-2 experiment; the code just needs to pin.)
- Provide `lscpu -e` guidance so the user picks distinct **physical** cores (avoid two hyperthreads of one core).

### 8.7 Baseline comparators

Same producer/consumer harness, different transport, for the "why lock-free" plot:
- `std::mutex` + `std::condition_variable` bounded queue (per consumer, or shared).
- (Optional) `boost::lockfree::spsc_queue` or a channel, if available.
Report the same stats; expect much worse tails, especially under load.

---

## 9. Experiment plan (produces the findings)

Each experiment is a config + a plot. All plots share the harness output format.

| E | Experiment | Independent variable | Plot | Expected finding |
|---|-----------|----------------------|------|------------------|
| E1 | Baseline distribution | — | 4-panel latency (hist linear/log + timeseries) | Tight body + tail structure |
| E2 | Latency vs load | mean rate (sweep) | p50/p99/p99.9 vs rate | Tail degrades past a knee |
| E3 | Burst correlation | Hawkes `alpha` (burstiness) | latency vs instantaneous rate | Spikes align with bursts |
| E4 | Fan-out scaling | consumer count 1,2,4,8 | p99.9 vs #consumers | Tail grows; hotspot shifts to `published_` line |
| E5 | False sharing | padding on/off (D4) | before/after distributions | Padding cuts tail materially |
| E6 | Memory ordering | acq/rel vs seq_cst (D6) | before/after | seq_cst adds measurable fence cost |
| E7 | Message size | 64 B vs 128 B slot | before/after | Multi-cache-line msg costs more |
| E8 | vs baseline queue | transport type | lock-free vs mutex tails | Mutex tail far worse under load |
| **E9 (phase 2)** | Jitter ladder | bare metal → isolcpus → nohz_full → PAUSE → turbo off | cascade of distributions + magic-trace attribution | Each rung's contribution to the tail |

E1–E8 run anywhere (WSL ok for methodology, bare metal for real numbers). E9 requires bare-metal kernel config and Intel PT (magic-trace) and is explicitly phase 2.

---

## 10. Output & plotting

- C++ writes one CSV (or binary) per run: `results/<experiment>/<config>.csv`, columns include `consumer_id, sample_ns`. A small `run_meta.json` (or header) records config, achieved rate, drops, stats.
- Python (`plots/`) reads these and regenerates all figures. Reuse the existing 4-panel latency plot (clipped linear + log-x hist, hexbin timeseries, log-y timeseries) as the per-run view; add per-experiment comparison plots (E2–E8).
- `bench/run_all.sh` runs the full E1–E8 matrix and regenerates every figure → true one-command reproduction.

---

## 11. Testing & correctness plan

Correctness is the headline signal for this project; treat it as first-class.

- **SPSC unit tests:** push/pop FIFO invariant; full/empty edges; wrap-around; 10⁷-iteration single-thread sanity; two-thread stress with a monotonic-sequence assertion (no gaps, no dups, no reorder).
- **SPMC broadcast tests:**
  - *No spurious gaps:* with a slow-enough producer (or large N), every consumer sees a contiguous sequence — assert no gaps.
  - *Correct gap accounting:* with a deliberately-lapped consumer, verify detected drops match `producer_seq − consumed − received`.
  - *Overwrite race:* stress at saturation; assert every *received* payload's `seq` matches its slot and body is internally consistent (e.g. a checksum field), i.e. never a torn message.
- **ThreadSanitizer** build (`-fsanitize=thread`) over the SPSC and a small SPMC run to catch data races and validate the memory-ordering claims. (Expect to annotate/￼understand any benign reports.)
- **Timing self-test:** `rdtsc` calibration reports ticks/ns; a known `sleep`-based sanity check that converted ns are sane.
- **CI (optional):** GitHub Actions builds, runs unit tests + a short E1 smoke run.

---

## 12. Repo layout

```
mdbus/
├── CMakeLists.txt
├── README.md                 # thesis, methodology, headline plots, findings
├── DESIGN.md                 # this file
├── include/mdbus/
│   ├── message.hpp
│   ├── spsc_ring.hpp
│   ├── spmc_ring.hpp
│   ├── source.hpp            # Source interface + Generator (Hawkes/Poisson/const)
│   ├── timing.hpp            # rdtsc + calibration
│   ├── stats.hpp             # percentiles, drops
│   └── affinity.hpp          # pin_to_core, topology helpers
├── src/
│   ├── generator.cpp
│   └── ... (impl if not header-only)
├── bench/
│   ├── bench_main.cpp        # config-driven runner (one binary, flags select experiment)
│   └── run_all.sh
├── tests/
│   ├── test_spsc.cpp
│   ├── test_spmc.cpp
│   └── test_timing.cpp
├── plots/
│   ├── plot_run.py           # 4-panel per-run view
│   └── plot_experiments.py   # E2–E8 comparisons
└── results/                  # generated CSVs + figures (gitignored except samples)
```

---

## 13. Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release   # -O3 -march=native -pthread
cmake --build build -j
ctest --test-dir build                       # correctness
./bench/run_all.sh                           # E1–E8 + all plots
```
Config surface (flags or a small config struct): ring size N, consumer cores, arrival model + params, target rate, run mode (paced/saturation), padding on/off, memory order, message size, warmup fraction, sample count.

---

## 14. Phasing

- **Phase 0 — Foundation:** Message, SPSC ring (D2–D4), rdtsc+calibration, per-run 4-panel plot, SPSC tests. Deliverable: clean SPSC baseline distribution.
- **Phase 1 — The bus & core study (main scope):** SPMC broadcast (D3, D5, gap detection), Hawkes/Poisson/const generator with paced+saturation modes, harness with coordinated-omission latency, experiments **E1–E8**, SPMC tests + TSan, README with findings. **This is the résumé-complete project.**
- **Phase 2 — Depth flexes (optional):** bare-metal jitter ladder **E9** (isolcpus/nohz_full/PAUSE/turbo) with magic-trace attribution; `Replayer` implementing `Source` for real data (Tardis crypto and/or Databento CME MBO); HdrHistogram; cross-process `shm_open`/`mmap` variant (ties to the OCaml/GIL talk).

**Scope discipline:** a finished, rigorous Phase 0+1 beats a sprawling half-built Phase 2. Ship Phase 1 fully — tests, plots, README — before starting any Phase 2 item.

---

## 15. Open knobs / notes for the implementer

- Decide `rdtscp` vs `lfence;rdtsc` for serialization; document the tradeoff (ordering vs overhead) and keep it consistent.
- Backoff strategy in the consumer spin (busy-spin vs `PAUSE` intrinsic `_mm_pause()`): make it a flag — `PAUSE` is itself a jitter-ladder rung (E9).
- The generator must not be the bottleneck. Pre-generate schedules; if RNG cost intrudes, generate payloads once and vary only `seq`/`intended_tsc`.
- Prefer a single bench binary whose flags select the experiment, so `run_all.sh` is just a matrix of invocations.
- Keep a `checksum`/canary field in `Message` (computed from body) so torn-message detection in tests is trivial.
- All shared counters that are read by one thread and written by another must be `std::atomic` with explicit ordering; everything thread-local stays plain.

---

*End of design doc.*
