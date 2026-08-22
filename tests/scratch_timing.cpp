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
    return 0;
}
