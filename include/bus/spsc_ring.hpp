#pragma once

// Bounded lock-free SPSC ring.
//
// Exactly one thread may push and exactly one may pop. Nothing defends against
// more, and that assumption is what buys the speed: with one writer per cursor
// no compare-exchange is needed anywhere.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bus {

// -DBUS_NO_PADDING packs the cursors into one line so E5 can measure what
// padding is worth rather than asserting it.
#ifdef BUS_NO_PADDING
inline constexpr size_t kCursorAlign = alignof(std::atomic<uint64_t>);
#else
inline constexpr size_t kCursorAlign = 64;
#endif

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2, "ring needs at least two slots");
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    using value_type = T;
    static constexpr size_t capacity = N;
    static constexpr bool   padded   = (kCursorAlign >= 64);

    bool try_push(const T& value) noexcept {
        // Own cursor, so no synchronisation needed to read what we last wrote.
        const uint64_t head = head_.load(std::memory_order_relaxed);
        // Acquire pairs with the consumer's release: guarantees its read of the
        // slot completed before we overwrite it.
        const uint64_t tail = tail_.load(std::memory_order_acquire);

        if (head - tail == N) return false;

        buf_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);

        if (head == tail) return false;

        out = buf_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate while both threads run: the answer can be stale before the
    // caller reads it. For tests and reporting, not control flow.
    uint64_t size_approx() const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }
    bool empty_approx() const noexcept { return size_approx() == 0; }
    bool full_approx()  const noexcept { return size_approx() == N; }

    uint64_t pushed() const noexcept { return head_.load(std::memory_order_acquire); }
    uint64_t popped() const noexcept { return tail_.load(std::memory_order_acquire); }

private:
    static constexpr uint64_t kMask = N - 1;

    // Total pushes and pops, never wrapped. Wrapped indices would make
    // head == tail mean either empty or full with no way to tell, and the usual
    // fixes are wasting a slot or keeping a separate synchronised count.
    alignas(kCursorAlign) std::atomic<uint64_t> head_{0};
    alignas(kCursorAlign) std::atomic<uint64_t> tail_{0};
    alignas(64)           std::array<T, N>      buf_{};
};

}  // namespace bus
