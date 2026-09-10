// Ping-pong variants, measured under identical conditions in one process.
//
// Each variant changes exactly one thing relative to the previous, so the
// difference is attributable. Run order is randomised per repetition to stop a
// warming machine from being mistaken for a faster variant.

#include "bus/affinity.hpp"
#include "bus/timing.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <x86intrin.h>

namespace {

constexpr int      kSlots = 4;
constexpr uint64_t kHops  = 400'000;
constexpr int      kCoreA = 4;
constexpr int      kCoreB = 6;

struct Res { double min, p50, p90, p99, p999, max; };

Res summarise(std::vector<uint64_t> v, const bus::TscClock& c) {
    v.erase(v.begin(), v.begin() + v.size() / 10);
    std::sort(v.begin(), v.end());
    auto q = [&](double p) { return c.to_ns(v[(size_t)(p * (v.size() - 1))]); };
    return {c.to_ns(v.front()), q(.5), q(.9), q(.99), q(.999), c.to_ns(v.back())};
}

// ---------------------------------------------------------------------------
// V0: the original. Two separate arrays, seq_cst everywhere, sample written
//     into a kHops-long vector inside the loop.
// ---------------------------------------------------------------------------
std::atomic<int> g_r1[kSlots], g_r2[kSlots];

Res v0_original(const bus::TscClock& clk) {
    for (int i = 0; i < kSlots; ++i) { g_r1[i] = 0; g_r2[i] = 0; }
    std::vector<uint64_t> samples(kHops);

    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_r1[i].store(val);
            while (g_r2[i].load() != val + 1) {}
            samples[h] = bus::rdtsc_ordered(cpu) - t0;
            val = g_r2[i].load() + 1;
            i = (i + 1) % kSlots;
        }
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            while (g_r1[i].load() <= g_r2[i].load()) {}
            g_r2[i].store(g_r1[i].load() + 1);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

// ---------------------------------------------------------------------------
// V1: same protocol, but the two rings live in one 64-byte-aligned struct so
//     the layout is chosen rather than left to the stack allocator.
// ---------------------------------------------------------------------------
struct alignas(64) Packed { std::atomic<int> ping[kSlots], pong[kSlots]; };
Packed g_packed;

Res v1_packed(const bus::TscClock& clk) {
    for (int i = 0; i < kSlots; ++i) { g_packed.ping[i] = 0; g_packed.pong[i] = 0; }
    std::vector<uint64_t> samples(kHops);
    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[i].store(val);
            while (g_packed.pong[i].load() != val + 1) {}
            samples[h] = bus::rdtsc_ordered(cpu) - t0;
            val = g_packed.pong[i].load() + 1;
            i = (i + 1) % kSlots;
        }
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            while (g_packed.ping[i].load() <= g_packed.pong[i].load()) {}
            g_packed.pong[i].store(g_packed.ping[i].load() + 1);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}


// ---------------------------------------------------------------------------
// V2a: packed + release/acquire ONLY. Spin loop still reads both atomics every
//      iteration, as the original does. Isolates the cost of seq_cst.
// ---------------------------------------------------------------------------
Res v2a_relacq_only(const bus::TscClock& clk) {
    using std::memory_order_acquire; using std::memory_order_release;
    for (int i = 0; i < kSlots; ++i) { g_packed.ping[i] = 0; g_packed.pong[i] = 0; }
    std::vector<uint64_t> samples(kHops);
    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[i].store(val, memory_order_release);
            while (g_packed.pong[i].load(memory_order_acquire) != val + 1) {}
            samples[h] = bus::rdtsc_ordered(cpu) - t0;
            val = g_packed.pong[i].load(memory_order_acquire) + 1;
            i = (i + 1) % kSlots;
        }
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            while (g_packed.ping[i].load(memory_order_acquire)
                   <= g_packed.pong[i].load(memory_order_acquire)) {}
            g_packed.pong[i].store(g_packed.ping[i].load(memory_order_acquire) + 1,
                                   memory_order_release);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

// ---------------------------------------------------------------------------
// V2b: original seq_cst, but with the consumer's spin loop hoisted. Isolates
//      the cost of the redundant second load in the spin.
// ---------------------------------------------------------------------------
Res v2b_hoist_only(const bus::TscClock& clk) {
    for (int i = 0; i < kSlots; ++i) { g_packed.ping[i] = 0; g_packed.pong[i] = 0; }
    std::vector<uint64_t> samples(kHops);
    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[i].store(val);
            while (g_packed.pong[i].load() != val + 1) {}
            samples[h] = bus::rdtsc_ordered(cpu) - t0;
            val = g_packed.pong[i].load() + 1;
            i = (i + 1) % kSlots;
        }
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            const int mine = g_packed.pong[i].load();
            int seen;
            while ((seen = g_packed.ping[i].load()) <= mine) {}
            g_packed.pong[i].store(seen + 1);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

// ---------------------------------------------------------------------------
// V2: packed layout + release/acquire instead of seq_cst. A seq_cst store
//     compiles to a locked xchg; a release store is a plain mov.
// ---------------------------------------------------------------------------
Res v2_relacq(const bus::TscClock& clk) {
    using std::memory_order_acquire; using std::memory_order_release;
    using std::memory_order_relaxed;
    for (int i = 0; i < kSlots; ++i) { g_packed.ping[i] = 0; g_packed.pong[i] = 0; }
    std::vector<uint64_t> samples(kHops);
    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[i].store(val, memory_order_release);
            while (g_packed.pong[i].load(memory_order_acquire) != val + 1) {}
            samples[h] = bus::rdtsc_ordered(cpu) - t0;
            val += 2;                      // known: pong is always val+1
            i = (i + 1) % kSlots;
        }
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            // Only this thread writes pong, so its value cannot change while
            // we wait: read it once instead of every spin iteration.
            const int mine = g_packed.pong[i].load(memory_order_relaxed);
            int seen;
            while ((seen = g_packed.ping[i].load(memory_order_acquire)) <= mine) {}
            g_packed.pong[i].store(seen + 1, memory_order_release);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

// ---------------------------------------------------------------------------
// V3: V2, but the sample is kept in a register and only written out after the
//     spin, to a slot in a small ring rather than streaming through 3 MB.
//     Tests whether the recording itself perturbs the measurement.
// ---------------------------------------------------------------------------
Res v3_nostream(const bus::TscClock& clk) {
    using std::memory_order_acquire; using std::memory_order_release;
    using std::memory_order_relaxed;
    for (int i = 0; i < kSlots; ++i) { g_packed.ping[i] = 0; g_packed.pong[i] = 0; }
    std::vector<uint64_t> samples(kHops);
    // Pre-fault every page so no page fault lands inside a measured interval.
    std::memset(samples.data(), 0, samples.size() * sizeof(uint64_t));

    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int i = 0, val = 1; uint32_t cpu = 0;
        uint64_t* out = samples.data();
        for (uint64_t h = 0; h < kHops; ++h) {
            const uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[i].store(val, memory_order_release);
            while (g_packed.pong[i].load(memory_order_acquire) != val + 1) {}
            const uint64_t t1 = bus::rdtsc_ordered(cpu);
            // Non-temporal store: writes straight to memory without pulling the
            // line into L1, so the sample stream stops evicting the ring line.
            _mm_stream_si64((long long*)(out + h), (long long)(t1 - t0));
            val += 2;
            i = (i + 1) % kSlots;
        }
        _mm_sfence();
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int i = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            const int mine = g_packed.pong[i].load(memory_order_relaxed);
            int seen;
            while ((seen = g_packed.ping[i].load(memory_order_acquire)) <= mine) {}
            g_packed.pong[i].store(seen + 1, memory_order_release);
            i = (i + 1) % kSlots;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

// ---------------------------------------------------------------------------
// V4: V3 with a single slot. Tests whether the 4-slot ring buys anything at
//     all in a closed loop where only one message is ever in flight.
// ---------------------------------------------------------------------------
Res v4_oneslot(const bus::TscClock& clk) {
    using std::memory_order_acquire; using std::memory_order_release;
    g_packed.ping[0] = 0; g_packed.pong[0] = 0;
    std::vector<uint64_t> samples(kHops);
    std::memset(samples.data(), 0, samples.size() * sizeof(uint64_t));

    std::thread a([&] {
        bus::pin_and_verify(kCoreA);
        int val = 1; uint32_t cpu = 0;
        uint64_t* out = samples.data();
        for (uint64_t h = 0; h < kHops; ++h) {
            const uint64_t t0 = bus::rdtsc_ordered(cpu);
            g_packed.ping[0].store(val, memory_order_release);
            while (g_packed.pong[0].load(memory_order_acquire) != val + 1) {}
            const uint64_t t1 = bus::rdtsc_ordered(cpu);
            _mm_stream_si64((long long*)(out + h), (long long)(t1 - t0));
            val += 2;
        }
        _mm_sfence();
    });
    std::thread b([&] {
        bus::pin_and_verify(kCoreB);
        int mine = 0;
        for (uint64_t h = 0; h < kHops; ++h) {
            int seen;
            while ((seen = g_packed.ping[0].load(memory_order_acquire)) <= mine) {}
            g_packed.pong[0].store(seen + 1, memory_order_release);
            mine = seen + 1;
        }
    });
    a.join(); b.join();
    return summarise(std::move(samples), clk);
}

struct Variant { const char* name; Res (*fn)(const bus::TscClock&); const char* what; };

}  // namespace

int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 3;
    const bus::TscClock clk = bus::calibrate_tsc(100);

    Variant vs[] = {
        {"v0 original",   v0_original, "two arrays, seq_cst, streaming samples"},
        {"v1 packed",     v1_packed,   "+ both rings in one aligned struct"},
        {"v2a rel/acq",   v2a_relacq_only, "+ release/acquire only"},
        {"v2b hoist",     v2b_hoist_only,  "+ hoisted spin load only (still seq_cst)"},
        {"v2 both",       v2_relacq,   "+ release/acquire AND hoisted spin"},
        {"v3 nt-store",   v3_nostream, "+ non-temporal sample store"},
        {"v4 one slot",   v4_oneslot,  "+ single slot instead of 4"},
    };
    constexpr int NV = sizeof(vs) / sizeof(vs[0]);
    std::vector<std::vector<Res>> all(NV);

    for (int r = 0; r < reps; ++r) {
        // Randomise order so thermal drift is not attributed to a variant.
        int order[NV];
        for (int i = 0; i < NV; ++i) order[i] = i;
        for (int i = NV - 1; i > 0; --i) {
            const int j = (int)((bus::rdtsc_relaxed() >> 4) % (uint64_t)(i + 1));
            std::swap(order[i], order[j]);
        }
        for (int k = 0; k < NV; ++k) {
            std::fprintf(stderr, "\rrep %d/%d: %-14s", r + 1, reps, vs[order[k]].name);
            all[order[k]].push_back(vs[order[k]].fn(clk));
        }
    }
    std::fprintf(stderr, "\r%40s\r", "");

    auto med = [](std::vector<double> x) {
        std::sort(x.begin(), x.end());
        return x[x.size() / 2];
    };
    std::printf("%-14s %8s %8s %8s %8s %9s %10s   %s\n",
                "variant", "min", "p50", "p90", "p99", "p99.9", "p99/p50", "change");
    std::printf("%s\n", std::string(100, '-').c_str());
    for (int i = 0; i < NV; ++i) {
        std::vector<double> mn, p50, p90, p99, p999;
        for (const Res& r : all[i]) {
            mn.push_back(r.min); p50.push_back(r.p50); p90.push_back(r.p90);
            p99.push_back(r.p99); p999.push_back(r.p999);
        }
        std::printf("%-14s %8.1f %8.1f %8.1f %8.1f %9.1f %10.2f   %s\n",
                    vs[i].name, med(mn), med(p50), med(p90), med(p99), med(p999),
                    med(p99) / med(p50), vs[i].what);
    }
    std::printf("\nmedian of %d repetitions, %llu hops each, cores %d <-> %d\n",
                reps, (unsigned long long)kHops, kCoreA, kCoreB);
    return 0;
}
