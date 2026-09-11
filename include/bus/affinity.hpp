#pragma once

// Core placement. Which core a thread lands on is a first-order effect on a
// hybrid part: the same rdtscp costs 11 ns on a P-core and 19 on an E-core,
// and the scheduler picks an E-core for a short-lived process by default.

#include <pthread.h>
#include <sched.h>

namespace bus {

// pthread_setaffinity_np returns the error number directly and leaves errno
// alone, unlike most of POSIX, so testing for -1 reports success on every
// failure. glibc's CPU_SET also silently drops an out-of-range index, leaving
// an empty mask the kernel then rejects, so this return value is the only
// place the problem is visible.
inline bool pin_to_core(int core) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline int current_cpu() noexcept {
    return sched_getcpu();
}

// A successful pin is a statement about the request, not about where the
// thread is executing: the mask applies at the next scheduling decision.
inline bool pin_and_verify(int core) noexcept {
    if (!pin_to_core(core)) return false;
    sched_yield();
    return current_cpu() == core;
}

}  // namespace bus
