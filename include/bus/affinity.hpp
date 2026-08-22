#pragma once

// Core placement.
//
// Which cores a producer and consumer run on is not a detail: on this machine
// the same rdtscp costs 10.96 ns on a P-core and 19.19 ns on an E-core, and the
// scheduler picks an E-core by default for a short-lived process. Two threads
// landing on SMT siblings of one physical core share L1 and L2, so their
// "cross-core" latency never crosses a core at all. Neither mistake announces
// itself in the results.

#include <pthread.h>
#include <sched.h>

namespace bus {

// Pin the calling thread to `cpu`. Returns true on success.
//
// A cpu_set_t is a fixed-size bitmask, one bit per logical CPU, manipulated
// through macros rather than by hand: CPU_ZERO(&set) clears it, CPU_SET(n,
// &set) sets bit n. Passing a set with several bits allows the thread to run on
// any of them; one bit pins it.
//
// The gotcha: pthread_setaffinity_np RETURNS the error number directly and does
// NOT set errno, unlike most of POSIX. So the test is `!= 0`, not `== -1`. The
// "_np" suffix means non-portable -- this is a glibc extension, not POSIX.

inline bool pin_to_core(int core) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    const int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return err == 0;
}


inline int current_cpu() noexcept {
    return sched_getcpu();
}

// Pin, then confirm the kernel actually honoured it.
//
// Worth doing separately because a successful pin_to_core is a statement about
// the *request*, not about where the thread is executing. The affinity mask is
// applied at the next scheduling decision, so a thread can still be observed on
// its old CPU immediately afterwards -- yielding once gives the scheduler the
// opportunity to move it.

inline bool pin_and_verify(int core) noexcept {

    if (!pin_to_core(core)) return false;
    sched_yield();
    return current_cpu() == core;
}

}  // namespace bus
