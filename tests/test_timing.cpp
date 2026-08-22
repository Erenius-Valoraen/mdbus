// Timing self-test (DESIGN.md S11): validates the instrument before anything is
// measured with it. Prints what it finds, then asserts the findings are sane --
// a benchmark built on a clock that silently lies is worse than no benchmark.
//
// Thresholds are deliberately loose. They exist to catch gross errors (integer
// division in the calibration, a stopped counter, a reversed subtraction), not
// to police precision, so that a busy machine does not produce a red build.

#include "bus/timing.hpp"

#include <cstdio>

namespace {

int failures = 0;

// Report a check without aborting, so one run surfaces every problem at once
// rather than stopping at the first.
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

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

    // ---- assertions --------------------------------------------------------
    std::printf("\n--- checks ---\n");

    // A plausible ratio. Integer division in calibrate_tsc would land this on a
    // whole number near 2.0; a stopped counter would give 0.
    check(clk.ticks_per_ns > 0.1 && clk.ticks_per_ns < 20.0,
          "ticks_per_ns is in a plausible range");

    // The counter must advance, and two back-to-back reads must not take
    // anything like a microsecond.
    check(t1 > t0, "ordered reads are monotonic");
    check((t1 - t0) < 10000, "ordered read costs less than 10k ticks");
    check(r1 > r0, "relaxed reads are monotonic");

    // The relaxed read skips a fence, so it must not cost more than the ordered
    // one. This is the claim the two functions exist to make.
    check((r1 - r0) <= (t1 - t0), "relaxed read is no more costly than ordered");

    // The two clocks must agree over a 50 ms window. 0.5% is far looser than
    // the ~0.01% actually observed, but tight enough that a 46%-wrong
    // calibration cannot slip through.
    {
        const uint64_t tsc_a  = bus::rdtsc_relaxed();
        const uint64_t mono_a = bus::monotonic_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t tsc_b  = bus::rdtsc_relaxed();
        const uint64_t mono_b = bus::monotonic_ns();
        const double via_tsc  = clk.to_ns(tsc_b - tsc_a);
        const double via_mono = static_cast<double>(mono_b - mono_a);
        const double err = 100.0 * (via_tsc - via_mono) / via_mono;
        check(err > -0.5 && err < 0.5, "TSC agrees with CLOCK_MONOTONIC to 0.5%");
    }

    // Not a failure -- DESIGN.md S8.5 says warn, do not crash. A machine without
    // invariant TSC can still run the harness; its numbers just mean less.
    if (!clk.invariant) {
        std::printf("  [WARN] no invariant TSC: results are not trustworthy\n");
    }
    if (clk.virtualized) {
        std::printf("  [WARN] hypervisor present: host may preempt the vCPU\n");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
