// Affinity tests: does pinning actually put the thread where it was asked, and
// does a bad request fail rather than silently doing nothing?

#include "bus/affinity.hpp"

#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    const unsigned n = std::thread::hardware_concurrency();
    std::printf("logical CPUs: %u\n\n", n);

    std::printf("--- current_cpu reports something sane ---\n");
    const int here = bus::current_cpu();
    std::printf("  running on cpu %d\n", here);
    check(here >= 0 && here < static_cast<int>(n), "current_cpu is in range");

    std::printf("\n--- pinning lands where asked ---\n");
    for (int cpu : {0, 2, 4, 6}) {
        if (cpu >= static_cast<int>(n)) continue;
        const bool pinned = bus::pin_and_verify(cpu);
        std::printf("  requested cpu %d, now on cpu %d\n", cpu, bus::current_cpu());
        check(pinned, "pin_and_verify succeeded");
    }

    std::printf("\n--- pinning is sticky ---\n");
    // Once pinned, repeated reads must agree. Without pinning the scheduler is
    // free to migrate between these calls; with it, it is not.
    {
        bus::pin_and_verify(4);
        bool stable = true;
        for (int i = 0; i < 100000; ++i) {
            if (bus::current_cpu() != 4) { stable = false; break; }
        }
        check(stable, "stayed on cpu 4 across 100k reads");
    }

    std::printf("\n--- a bad request fails loudly ---\n");
    // Asking for a CPU that does not exist must report failure. A silent
    // no-op here is how a benchmark ends up unpinned without anyone noticing.
    check(!bus::pin_to_core(9999), "pinning to a nonexistent cpu returns false");

    std::printf("\n--- each thread pins independently ---\n");
    // Affinity is per-thread, not per-process, which is what makes a pinned
    // producer and a pinned consumer possible in one program.
    {
        std::vector<int> landed(2, -1);
        std::thread a([&] { bus::pin_and_verify(2); landed[0] = bus::current_cpu(); });
        std::thread b([&] { bus::pin_and_verify(6); landed[1] = bus::current_cpu(); });
        a.join();
        b.join();
        std::printf("  thread a -> cpu %d, thread b -> cpu %d\n", landed[0], landed[1]);
        check(landed[0] == 2 && landed[1] == 6, "two threads pinned to different cpus");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
