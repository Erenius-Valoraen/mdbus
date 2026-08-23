// SPSC ring correctness (DESIGN.md S11).
//
// Single-threaded checks establish the FIFO and boundary behaviour; the
// two-thread stress asserts the invariant that matters -- every message arrives
// exactly once, in order, intact.

#include "bus/affinity.hpp"
#include "bus/message.hpp"
#include "bus/spsc_ring.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bus::Message make(uint64_t seq) {
    bus::Message m{};
    m.seq       = seq;
    m.send_tsc  = seq * 3;
    m.price     = 4250000000 + static_cast<int64_t>(seq);
    m.size      = 100 + seq;
    m.symbol_id = 7;
    m.stamp();
    return m;
}

}  // namespace

int main() {
    std::printf("cursors are %s\n\n",
                bus::SpscRing<bus::Message, 8>::padded ? "padded onto separate cache lines"
                                                       : "PACKED (BUS_NO_PADDING)");

    std::printf("--- empty and full boundaries ---\n");
    {
        bus::SpscRing<int, 4> r;
        int out = -1;
        check(!r.try_pop(out), "pop on an empty ring fails");
        check(r.try_push(1) && r.try_push(2) && r.try_push(3) && r.try_push(4),
              "4 pushes into a 4-slot ring succeed");
        check(!r.try_push(5), "the 5th push fails -- ring is full, not overwriting");
        check(r.size_approx() == 4, "size reports 4");
        check(r.try_pop(out) && out == 1, "pop returns the first value");
        check(r.try_push(5), "a push succeeds again after one pop");
    }

    std::printf("\n--- FIFO order ---\n");
    {
        bus::SpscRing<int, 8> r;
        for (int i = 0; i < 8; ++i) r.try_push(i);
        bool ordered = true;
        for (int i = 0; i < 8; ++i) {
            int out = -1;
            if (!r.try_pop(out) || out != i) { ordered = false; break; }
        }
        check(ordered, "values come out in the order they went in");
    }

    std::printf("\n--- wrap-around ---\n");
    {
        // Push and pop far more than capacity, so the slot index wraps many
        // times. Catches an index that grows without masking, or a full/empty
        // test that breaks once the cursors exceed N.
        bus::SpscRing<int, 4> r;
        bool ok = true;
        for (int i = 0; i < 10'000; ++i) {
            int out = -1;
            if (!r.try_push(i) || !r.try_pop(out) || out != i) { ok = false; break; }
        }
        check(ok, "10k push/pop cycles through a 4-slot ring");
        check(r.pushed() == 10'000 && r.popped() == 10'000, "cursors counted every operation");
    }

    std::printf("\n--- single-threaded volume ---\n");
    {
        static bus::SpscRing<bus::Message, 1024> r;
        bool ok = true;
        for (uint64_t i = 0; i < 1'000'000; ++i) {
            bus::Message out{};
            if (!r.try_push(make(i)) || !r.try_pop(out)) { ok = false; break; }
            if (out.seq != i || !out.verify())            { ok = false; break; }
        }
        check(ok, "1M messages round-trip intact");
    }

    std::printf("\n--- two threads, pinned to separate physical P-cores ---\n");
    {
        constexpr uint64_t kCount = 2'000'000;
        static bus::SpscRing<bus::Message, 1024> r;

        std::atomic<uint64_t> gaps{0}, torn{0}, received{0};
        std::atomic<bool> consumer_ready{false};

        std::thread producer([&] {
            bus::pin_and_verify(4);
            for (uint64_t i = 0; i < kCount; ++i) {
                const bus::Message m = make(i);
                while (!r.try_push(m)) {}          // spin while full
            }
        });

        std::thread consumer([&] {
            bus::pin_and_verify(6);
            consumer_ready.store(true, std::memory_order_release);
            uint64_t expected = 0;
            bus::Message out{};
            while (expected < kCount) {
                if (!r.try_pop(out)) continue;     // spin while empty
                if (out.seq != expected) gaps.fetch_add(1, std::memory_order_relaxed);
                if (!out.verify())       torn.fetch_add(1, std::memory_order_relaxed);
                ++expected;
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });

        producer.join();
        consumer.join();

        std::printf("  received %llu of %llu\n",
                    (unsigned long long)received.load(), (unsigned long long)kCount);
        check(received.load() == kCount, "every message arrived");
        check(gaps.load() == 0,          "no gaps, no duplicates, no reordering");
        check(torn.load() == 0,          "no torn messages");
        check(r.empty_approx(),          "ring drained");
        check(consumer_ready.load(),     "consumer ran");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
