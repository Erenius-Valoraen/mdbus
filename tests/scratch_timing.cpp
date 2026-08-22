// Scratch driver for S1.1. Not part of the real test suite -- that arrives once
// there is something worth asserting. Build without cmake:
//
//   g++ -O2 -std=c++20 -Iinclude tests/scratch_timing.cpp -o /tmp/t && /tmp/t

#include "bus/timing.hpp"

#include <cstdio>

int main() {
    std::printf("invariant TSC : %s\n",
                bus::has_invariant_tsc() ? "yes" : "NO  <-- TSC is not a clock here");
    std::printf("hypervisor    : %s\n",
                bus::running_under_hypervisor() ? "yes <-- vCPU, host may preempt" : "no");
    std::printf("\n");

    uint32_t cpu_a = 0, cpu_b = 0;

    const uint64_t t0 = bus::rdtsc_ordered(cpu_a);
    const uint64_t t1 = bus::rdtsc_ordered(cpu_b);

    std::printf("t0            = %llu\n", (unsigned long long)t0);
    std::printf("t1            = %llu\n", (unsigned long long)t1);
    std::printf("delta         = %lld ticks\n", (long long)(t1 - t0));
    std::printf("cpu           = %u -> %u%s\n", cpu_a, cpu_b,
                cpu_a == cpu_b ? "" : "   <-- MIGRATED");

    // Deliberately two statements. In C++ the order in which the operands of
    // `a() - b()` are evaluated is unspecified, so writing it as one expression
    // could legally sample the second call first and yield a negative delta.
    const uint64_t r0 = bus::rdtsc_relaxed();
    const uint64_t r1 = bus::rdtsc_relaxed();
    std::printf("relaxed delta = %lld ticks\n", (long long)(r1 - r0));
    // ---- S1.3: calibration + self-test -------------------------------------
    const bus::TscClock clk = bus::calibrate_tsc(100);
    std::printf("\n--- calibration ---\n");
    std::printf("ticks_per_ns  = %.6f\n", clk.ticks_per_ns);
    std::printf("tsc frequency = %.3f MHz   (kernel said 2918.400)\n",
                clk.tsc_freq_mhz());
    std::printf("ordered read  = %.2f ns\n", clk.to_ns(t1 - t0));
    std::printf("relaxed read  = %.2f ns\n", clk.to_ns(r1 - r0));

    // Self-test: sleep a known duration and see whether the TSC agrees. The
    // sleep will overshoot -- the OS wakes you late -- so this checks that the
    // two clocks agree with each other, not that the sleep was punctual.
    std::printf("\n--- self-test: do the two clocks agree? ---\n");
    for (int ms : {5, 20, 50}) {
        const uint64_t tsc_a  = bus::rdtsc_relaxed();
        const uint64_t mono_a = bus::monotonic_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        const uint64_t tsc_b  = bus::rdtsc_relaxed();
        const uint64_t mono_b = bus::monotonic_ns();

        const double via_tsc  = clk.to_ns(tsc_b - tsc_a) / 1e6;
        const double via_mono = static_cast<double>(mono_b - mono_a) / 1e6;
        const double err_pct  = 100.0 * (via_tsc - via_mono) / via_mono;

        std::printf("sleep(%2d ms): tsc says %8.4f ms, monotonic says %8.4f ms"
                    "  -> %+.4f %%%s\n",
                    ms, via_tsc, via_mono, err_pct,
                    (err_pct > -0.05 && err_pct < 0.05) ? "  ok" : "  <-- OFF");
    }
    return 0;
}
