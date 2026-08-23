# mdbus

A lock-free market data bus. One producer thread hands 64 byte messages to consumer
threads pinned on other cores, and the point of the project is measuring how long that
takes, with most of the attention on the worst cases rather than the average.

It started from the cross-core ping-pong in Jane Street's talk on controlling jitter,
where two threads bounce an integer back and forth to measure how long a cache line
takes to move between cores. That toy is the atom of a real market data bus, so this
repo builds the thing it stands for and then looks at where the two-core picture stops
being a good model once you add real load, real fan-out and real message sizes.

The original ping-pong is still in [`legacy/`](legacy/) if you want to see where this
came from.

## Where it is right now

Five million messages from core 4 to core 6 through a 1024 slot ring, on an i7-13700H
running Arch. No gaps, no duplicates, no reordering, no torn payloads.

![latency distribution](results/spsc_padded/latency.png)

| | |
|---|---|
| min | 41.8 ns |
| p50 | 154.2 ns |
| p90 | 209.7 ns |
| p99 | 1.0 µs |
| p99.9 | 16.9 µs |
| max | 87.0 µs |
| throughput | 18.1 M msg/s (1156 MB/s) |

The body of that distribution is the real cost of moving a cache line between two
physical cores, and it is the number the design earns. The tail is mostly not the ring,
and I would rather say why than quote it as if it were.

Two things are in there. The first is queueing: this run is in saturation mode, so the
producer sends as fast as it can and the ring backs up behind the consumer. With 1024
slots draining at roughly 50 ns each there is about 50 µs of possible queueing delay
sitting in the buffer, which is the right order of magnitude for what the tail shows.
The second is the operating system. Nothing on this machine is isolated yet, SMT is on,
and there is a desktop running underneath, so timer ticks and scheduler decisions land
in the middle of the measurement. You can see them in the bottom right panel as spikes
in the window max while the median stays flat.

Both are fixable and neither is a property of the ring. Pacing the producer against a
pre-generated arrival schedule removes the queueing and makes the measurement
coordinated-omission correct, and `isolcpus` plus `nohz_full` removes most of the rest.
Both are on the roadmap below.

## Reproducing it

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/spsc_latency --messages 5000000 --out results/spsc_padded
python plots/plot_run.py results/spsc_padded
```

Every run writes a `run_meta.json` next to its samples holding the configuration and
the environment it ran in: calibrated TSC frequency, whether the TSC is invariant,
whether a hypervisor is present, which cores were used, and whether the ring cursors
were padded. You cannot force a machine to be in a good state from inside a benchmark,
but you can make it impossible to end up with a latency number that has lost track of
the conditions that produced it.

## How the ring works

The ring is single producer, single consumer, bounded, and never takes a lock or
allocates. The producer only stops if the ring is genuinely full.

There are two counters. `head_` is the total number of pushes ever, `tail_` is the total
number of pops ever, and neither of them wraps back to zero. Everything else comes out
of those two numbers: the count waiting is `head_ - tail_`, empty is `head_ == tail_`,
full is `head_ - tail_ == N`. The obvious alternative is to store wrapped indices, but
then `head == tail` means either empty or completely full and you cannot tell which,
so you end up wasting a slot or keeping a separate count that needs its own
synchronisation. Running totals avoid that, and a `uint64_t` counter does not run out
inside any run you will ever do.

Capacity is a power of two, which lets the slot index be `seq & (N-1)` instead of
`seq % N`. Integer division on x86 costs something like 20 to 40 cycles and does not
pipeline well, and an AND costs one. There is a `static_assert` on the power-of-two
property because the optimisation is only valid if the assumption holds.

The memory ordering follows one rule applied symmetrically. Each thread reads its own
cursor with `relaxed` because it is the only writer, reads the other thread's cursor
with `acquire`, and writes its own with `release`. The release on the producer's store
to `head_` is what stops the message body write from sinking below it, and the matching
acquire on the consumer's load is what makes that body visible. The less obvious one is
the producer's acquire on `tail_`, which guards the reverse direction: it guarantees the
consumer has finished reading a slot before the producer overwrites it.

There is no compare-exchange anywhere. With exactly one writer per cursor a plain load
and a plain store are enough, and that restriction is the whole reason an SPSC ring
beats a general purpose queue. The contract is written at the top of the header and
deliberately not enforced, since enforcing it would cost exactly what the design is
buying.

## Measurement

The timing layer came before anything it measures, which turned out to matter more than
I expected.

Timestamps come from `rdtscp` followed by an `lfence`, not from `std::chrono`.
`rdtscp` waits for previously issued instructions to have executed before it samples the
counter and the fence stops later instructions from being hoisted above the read, which
between them are what make the pair of reads an actual stopwatch rather than a
suggestion. There is also a relaxed variant using bare `rdtsc` for places where a few
nanoseconds of slop does not matter, and knowing which one to reach for is part of the
point.

Measured on a P-core:

| | |
|---|---|
| `rdtsc_ordered` (rdtscp + lfence) | 11.0 ns |
| `rdtsc_relaxed` (bare rdtsc) | 5.5 ns |
| `std::chrono::steady_clock::now()` | 20 to 25 ns |

The reason for characterising the instrument first is that the thing being measured is
about 150 ns, so an unknown 20 ns of overhead is a meaningful fraction of it. Now it is
a known quantity instead.

The TSC frequency is measured at startup against `CLOCK_MONOTONIC` rather than taken
from the kernel's boot-time estimate, and it lands within about 13 parts per million of
what the kernel reports. `ctest` fails the build if that check stops holding, if the
CPU turns out not to have an invariant TSC, or if the two clocks stop agreeing.
Percentiles are nearest-rank rather than interpolated, so a reported p99.9 is a latency
that was actually observed.

## Core placement

The i7-13700H is a hybrid chip. Six P-cores (Raptor Cove, SMT, 4.8 to 5.0 GHz, CPUs 0
to 11) and eight E-cores (Gracemont, no SMT, 3.7 GHz, CPUs 12 to 19).

```
CPU:   0  1  |  2  3  |  4  5  |  6  7  |  8  9  | 10 11 | 12..19
CORE:  0     |  1     |  2     |  3     |  4     |  5    | 6..13
     4.8GHz    4.8      5.0      5.0      4.8      4.8     3.7 (E)
     └─SMT─┘  └─SMT─┘  └─SMT─┘  └─SMT─┘  └─SMT─┘  └SMT─┘   no SMT
```

Which core you land on is a first order effect. The same `rdtscp` and `lfence` sequence
measures 11.0 ns on a P-core and 19.2 ns on an E-core, a factor of 1.7 for identical
instructions. The frequency ratio only accounts for part of that and the rest is
microarchitecture, since the two core types are genuinely different designs that happen
to share an instruction set. Left alone the scheduler picks an E-core for a short-lived
benchmark process, so the default is the wrong answer.

Two threads on SMT siblings of the same physical core share L1 and L2, so a message
passing between them never leaves the core and the number you get is flattering and
meaningless. Benchmarks therefore pin one thread per physical P-core, drawn from
`{0, 2, 4, 6, 8, 10}`, and `pin_to_core` checks the return code because
`pthread_setaffinity_np` returns the error number directly rather than -1 like most of
POSIX, so testing it the usual way silently reports success on every failure.

## Things I found along the way

**Padding is not free and not always right.** The reflexive fix for two hot atomics is
to put them on separate cache lines. I measured that on the original ping-pong and it
made things about three times worse at the floor, because in a closed loop the two
threads strictly alternate and never touch the line at the same time, so there is no
false sharing to remove. All four slots of the ring fit in one line, which meant
rotating between them cost nothing until padding split them across eight lines. False
sharing needs two threads making progress independently, and a ping-pong by
construction does not have that.

**The compiler will delete your benchmark.** An early version of the consumer read a
slot, compared its sequence number, and did nothing with the result. GCC removed the
entire loop body, leaving two instructions that only checked the shutdown flag.
ThreadSanitizer reported no races, which was true and useless, because there was no
longer any code to race.

**Padding the ring cursors currently shows no measurable difference.** Three runs each
way and the ranges overlap completely on this machine. I think the reason is that
`try_push` reads `tail_` on every single call and `try_pop` reads `head_` on every call,
so both cache lines move between the cores every operation no matter how they are laid
out. Padding should only start to pay once each side can make progress without touching
the other's cursor, which needs the cached-cursor optimisation that is not in yet. That
is a hypothesis and the experiment to settle it is E5 on the roadmap.

**When a transport saturates, latency does not degrade gracefully.** A mutex and
condition variable queue on the same workload costs roughly 5 times more per message,
which is not surprising. What is more interesting is what that does to latency: it
cannot keep up, so its queue sits permanently full, and every message ends up waiting
behind a full buffer. Observed p50 went from around 120 ns to around 196 µs, and almost
all of that is backlog rather than per-message cost. This is the failure mode that
matters for market data, because the moment you fall behind is a burst, and a burst is
exactly when being late is expensive.

## Roadmap

| Step | | |
|---|---|---|
| S0 | Repo scaffold, CMake | done |
| S1 | `timing.hpp`, rdtsc, invariant-TSC check, calibration | done |
| S2 | `message.hpp`, `affinity.hpp` | done |
| S3 | `spsc_ring.hpp`, tests, ThreadSanitizer | done |
| S4 | `stats.hpp`, CSV and run metadata, `plots/plot_run.py` | done |
| S5 | `spmc_ring.hpp`, broadcast to N consumers with gap detection | next |
| S6 | `source.hpp`, constant / Poisson / Hawkes arrival models, paced mode | |
| S7 | Config-driven benchmark runner | |
| S8 | Mutex and condition variable comparator | |
| S9 | Experiments E1 to E8, `run_all.sh`, comparison plots | |
| S10 | Write-up of findings | |

The experiments themselves are described in [`DESIGN.md`](DESIGN.md), which also has the
reasoning behind the decisions above in more detail.

## Layout

```
include/bus/     timing.hpp  message.hpp  affinity.hpp  spsc_ring.hpp  stats.hpp
bench/           spsc_latency.cpp
tests/           test_timing  test_affinity  test_message  test_spsc  (+ a TSan build)
plots/           plot_run.py
legacy/          the ping-pong this grew out of, and a first attempt at the bus
results/         generated CSVs, metadata and figures (samples are gitignored)
```

## Caveats

Numbers here are from bare metal Arch on an i7-13700H with the CPU governor set to
performance, but with no core isolation, SMT still enabled, and a normal desktop
running. The median and the throughput are trustworthy. The tail has operating system
noise in it and should be read as an upper bound on what the ring costs rather than a
measurement of it. Getting a defensible tail number needs `isolcpus`, `nohz_full` and a
paced producer, which is what S6 and the E9 jitter ladder are for.
