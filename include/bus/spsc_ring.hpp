#pragma once

// Bounded lock-free single-producer / single-consumer ring.
//
// CONTRACT: exactly one thread may call try_push, exactly one may call try_pop.
// Two producers, or two consumers, is undefined behaviour -- nothing here
// defends against it, and that assumption is precisely what buys the speed:
// with one writer per cursor, no compare-and-swap is ever needed.
//
// Never blocks, never allocates, never takes a lock. A full ring refuses the
// push; an empty ring refuses the pop.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bus {

// Cursor alignment. Padding the two cursors onto separate cache lines is the
// default; -DBUS_NO_PADDING packs them together so experiment E5 can measure
// what that costs, rather than asserting it.
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

    // Producer side. Returns false if the ring is full; the caller decides
    // whether to retry, drop, or back off.
    bool try_push(const T& value) noexcept {
        // Own cursor: this thread is its only writer, so no synchronisation is
        // needed to read a value we ourselves last wrote.
        const uint64_t head = head_.load(std::memory_order_relaxed);

        // Other thread's cursor. Acquire, so that the consumer's read of the
        // slot -- sequenced before its release store to tail_ -- happens-before
        // the write below. Without it we could overwrite a slot mid-read.
        const uint64_t tail = tail_.load(std::memory_order_acquire);

        if (head - tail == N) return false;          // full

        buf_[head & kMask] = value;

        // Release: publishes the slot write above to any consumer that
        // acquire-loads head_ and sees this value.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the ring is empty.
    bool try_pop(T& out) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);   // own cursor
        const uint64_t head = head_.load(std::memory_order_acquire);   // pairs with push's release

        if (head == tail) return false;              // empty

        out = buf_[tail & kMask];

        // Release: tells the producer this slot is free, and orders the read
        // above before any subsequent overwrite of it.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Diagnostics. Inherently approximate while both threads are running --
    // the answer can be stale before the caller reads it. For tests and
    // reporting, never for control flow.
    uint64_t size_approx() const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }
    bool empty_approx() const noexcept { return size_approx() == 0; }
    bool full_approx()  const noexcept { return size_approx() == N; }

    // Total pushes and pops since construction. Monotonic, never reset.
    uint64_t pushed() const noexcept { return head_.load(std::memory_order_acquire); }
    uint64_t popped() const noexcept { return tail_.load(std::memory_order_acquire); }

private:
    static constexpr uint64_t kMask = N - 1;

    // The cursors count TOTAL pushes and pops -- they never wrap back to zero,
    // and the slot index is derived from them. See the header comment in
    // spsc_ring notes: this is what makes empty and full distinguishable.
    alignas(kCursorAlign) std::atomic<uint64_t> head_{0};   // producer writes
    alignas(kCursorAlign) std::atomic<uint64_t> tail_{0};   // consumer writes
    alignas(64)           std::array<T, N>      buf_{};
};

}  // namespace bus
