#pragma once

// Low-overhead timing primitives built on the x86 time-stamp counter.
//
// The TSC is a 64-bit per-core counter that `rdtsc` reads directly, with no
// kernel transition. On a CPU with invariant TSC it advances at a fixed
// reference rate independent of core frequency, which makes it usable as a
// clock rather than merely as a cycle counter. Whether this CPU has that
// property is checked at startup (S1.2) rather than assumed.

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <cpuid.h>       // __get_cpuid
#include <x86intrin.h>   // __rdtsc, __rdtscp, _mm_lfence

namespace bus {

// Read the TSC with no ordering guarantees whatsoever.
//
// Instructions may be reordered freely across this call in both directions, so
// it is only appropriate where the surrounding work is large enough that a few
// tens of cycles of slop do not matter -- e.g. timestamping a message body that
// is about to be published, where the store itself is the ordering point.
//
inline uint64_t rdtsc_relaxed() noexcept {
    return __rdtsc();
}

// Read the TSC, ordered against earlier and later instructions, and report the
// core the read happened on.
//
// `rdtscp` waits for previously issued instructions to have executed before
// sampling the counter, which is the guarantee `rdtsc` lacks. It does not stop
// *later* instructions from being hoisted above it, so an lfence follows.
//
// `cpu_id` receives IA32_TSC_AUX, which Linux populates with the current CPU
// number. Comparing it across a pair of reads detects a thread migration that
// would otherwise silently corrupt the sample -- the TSC is per-core, and while
// invariant TSC keeps cores closely synchronised, a migration mid-measurement
// means the two endpoints came from different counters.
//
inline uint64_t rdtsc_ordered(uint32_t& cpu_id) noexcept {
    const uint64_t t = __rdtscp(&cpu_id);
    _mm_lfence();
    return t;
}


// ---------------------------------------------------------------------------
// Environment checks (S1.2)
//
// Everything above treats the TSC as a clock. That is only true on a CPU with
// an *invariant* TSC, where the counter is driven from a fixed reference rate
// rather than the core's own (heavily variable) clock. Rather than assume it,
// ask the hardware.
// ---------------------------------------------------------------------------

// True if the CPU advertises an invariant TSC.
//
// CPUID extended leaf 0x80000007, EDX bit 8. Extended leaves are not universal,
// so __get_cpuid's return value has to be honoured -- on a CPU without the leaf
// the output variables are left untouched, and reading them would be reading
// uninitialised memory.
//
// TODO(S1.2):
//   1. declare four unsigned variables for the output registers
//   2. call __get_cpuid with leaf 0x80000007; if it returns 0, return false
//   3. return bit 8 of edx as a bool
inline bool has_invariant_tsc() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007,&eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (edx >> 8) & 1;
}

// True if a hypervisor is present (CPUID leaf 0x1, ECX bit 31).
//
// This bit is set by the hypervisor itself, by convention, on every major VMM.
// It matters here because under virtualisation the "core" a thread is pinned to
// is a virtual CPU that the host may schedule anywhere and preempt at any time
// -- invisible from inside the guest, and a direct contributor to the latency
// tail this project measures. Not a failure; a caveat worth printing.
//
// TODO(S1.2): same shape as above, but a standard leaf and a different bit.
inline bool running_under_hypervisor() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
     return (ecx >> 31) & 1;
}


// ---------------------------------------------------------------------------
// Calibration (S1.3)
//
// The TSC counts ticks; every reported figure is in nanoseconds. The bridge is
// a ticks-per-nanosecond ratio, measured at startup against CLOCK_MONOTONIC
// rather than taken from the kernel's boot-time estimate -- which under a
// hypervisor is itself second-hand.
// ---------------------------------------------------------------------------

// CLOCK_MONOTONIC as a flat nanosecond count.
//
// MONOTONIC rather than REALTIME: REALTIME is wall-clock time and can step
// backwards when NTP corrects it, which would make a duration negative.
//
// TODO(S1.3):
//   1. declare a `timespec ts;`
//   2. clock_gettime(CLOCK_MONOTONIC, &ts);
//   3. return seconds converted to ns, plus the ns field
inline uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    return ns;
}

// Everything needed to interpret a raw tick count, plus the environment flags
// that say whether those ticks mean anything. Carried alongside every result so
// a run can never be reported without the conditions that produced it.
struct TscClock {
    double ticks_per_ns = 0.0;
    double ns_per_tick  = 0.0;   // reciprocal, precomputed: see below
    bool   invariant    = false;
    bool   virtualized  = false;

    // Convert a raw tick delta to nanoseconds.
    //
    // Multiplying by a stored reciprocal rather than dividing by ticks_per_ns
    // is deliberate: an FP divide is ~4x the latency of a multiply, and this
    // runs once per recorded sample when a run is post-processed.
    double to_ns(uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * ns_per_tick;
    }

    double tsc_freq_mhz() const noexcept { return ticks_per_ns * 1000.0; }
};

// Measure the TSC rate by racing it against CLOCK_MONOTONIC over `interval_ms`.
//
// The sleep duration does not need to be accurate -- both clocks are read
// before and after, so an overshooting sleep is measured, not assumed. Longer
// intervals dilute the fixed read-skew between the two clocks: ~30 ns of skew
// is 3% of a 1 us interval but 0.00003% of a 100 ms one.

inline TscClock calibrate_tsc(unsigned interval_ms = 100) noexcept {
    TscClock c;

    uint64_t mono_0 = monotonic_ns();
    uint64_t tsc_0 = rdtsc_relaxed();
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    uint64_t mono_1 = monotonic_ns();
    uint64_t tsc_1 = rdtsc_relaxed();

    c.ticks_per_ns = static_cast<double>(tsc_1 - tsc_0) / (mono_1 - mono_0);
    c.ns_per_tick = 1.0 / c.ticks_per_ns;
    c.invariant = has_invariant_tsc();
    c.virtualized = running_under_hypervisor();


    return c;
}

}  // namespace bus
