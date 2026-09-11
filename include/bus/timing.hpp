#pragma once

// TSC-based timing. The counter is per-core and advances at a fixed rate
// independent of the core clock, provided the CPU has invariant TSC, which
// has_invariant_tsc() checks rather than assumes.

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <cpuid.h>
#include <x86intrin.h>

namespace bus {

// No ordering guarantees. Fine where the surrounding stores already order
// things, or where a few ns of slop does not matter.
inline uint64_t rdtsc_relaxed() noexcept {
    return __rdtsc();
}

// rdtscp waits for earlier instructions to execute; the fence stops later ones
// being hoisted above the read. Neither half is sufficient alone.
//
// cpu_id receives IA32_TSC_AUX, which Linux fills with the CPU number, so
// comparing it across a pair of reads catches a migration mid-measurement.
inline uint64_t rdtsc_ordered(uint32_t& cpu_id) noexcept {
    const uint64_t t = __rdtscp(&cpu_id);
    _mm_lfence();
    return t;
}

// CPUID 0x80000007, EDX bit 8. The leaf is not universal, so the return value
// has to be honoured: on a CPU without it the outputs are left untouched and
// reading them is reading uninitialised memory.
inline bool has_invariant_tsc() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx)) return false;
    return (edx >> 8) & 1;
}

// Set by the hypervisor itself, by convention, on every major VMM. Matters
// because a pinned vCPU can still be descheduled by the host invisibly.
inline bool running_under_hypervisor() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx >> 31) & 1;
}

// MONOTONIC rather than REALTIME: REALTIME steps backwards when NTP corrects
// it, which would make a duration negative.
inline uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

struct TscClock {
    double ticks_per_ns = 0.0;
    double ns_per_tick  = 0.0;
    bool   invariant    = false;
    bool   virtualized  = false;

    // Multiply by the stored reciprocal rather than dividing: an FP divide is
    // ~4x the latency of a multiply and this runs once per recorded sample.
    double to_ns(uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * ns_per_tick;
    }

    double tsc_freq_mhz() const noexcept { return ticks_per_ns * 1000.0; }
};

// Race the TSC against CLOCK_MONOTONIC. The sleep does not need to be accurate
// since both clocks are read at both ends, but a longer interval dilutes the
// fixed read-skew between them: ~30 ns is 3% of a microsecond and nothing at
// 100 ms. Read order is the same at both ends so the skew largely cancels.
inline TscClock calibrate_tsc(unsigned interval_ms = 100) noexcept {
    TscClock c;

    const uint64_t mono_0 = monotonic_ns();
    const uint64_t tsc_0  = rdtsc_relaxed();
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    const uint64_t mono_1 = monotonic_ns();
    const uint64_t tsc_1  = rdtsc_relaxed();

    c.ticks_per_ns = static_cast<double>(tsc_1 - tsc_0) / (mono_1 - mono_0);
    c.ns_per_tick  = 1.0 / c.ticks_per_ns;
    c.invariant    = has_invariant_tsc();
    c.virtualized  = running_under_hypervisor();
    return c;
}

}  // namespace bus
