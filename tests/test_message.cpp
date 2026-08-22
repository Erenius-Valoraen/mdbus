// Message layout and torn-message detection.
//
// The layout checks are duplicated by static_asserts in the header (which fail
// the build); repeating them here prints the actual offsets, which is what you
// want when one of them starts failing.

#include "bus/message.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bus::Message make(uint64_t seq) {
    bus::Message m{};
    m.seq          = seq;
    m.intended_tsc = 1'000'000 + seq;
    m.send_tsc     = 1'000'005 + seq;
    m.price        = 42'50000000;          // 42.5 at 1e-8
    m.size         = 100 + seq;
    m.symbol_id    = 7;
    m.side         = static_cast<uint8_t>(bus::Side::Bid);
    m.level        = 1;
    m.flags        = static_cast<uint16_t>(bus::UpdateType::Add);
    m.stamp();
    return m;
}

}  // namespace

int main() {
    std::printf("--- layout ---\n");
    std::printf("  sizeof  = %zu\n", sizeof(bus::Message));
    std::printf("  alignof = %zu\n", alignof(bus::Message));
    std::printf("  offsets: seq=%zu intended=%zu send=%zu price=%zu size=%zu\n"
                "           symbol=%zu side=%zu level=%zu flags=%zu\n",
                offsetof(bus::Message, seq),
                offsetof(bus::Message, intended_tsc),
                offsetof(bus::Message, send_tsc),
                offsetof(bus::Message, price),
                offsetof(bus::Message, size),
                offsetof(bus::Message, symbol_id),
                offsetof(bus::Message, side),
                offsetof(bus::Message, level),
                offsetof(bus::Message, flags));

    check(sizeof(bus::Message) == 64, "Message is exactly one cache line");
    check(alignof(bus::Message) == 64, "Message is cache-line aligned");

    // An array of Messages must have every element on its own line -- if the
    // stride were not 64 the ring's slots would straddle lines.
    {
        bus::Message arr[4]{};
        const auto stride = reinterpret_cast<char*>(&arr[1]) - reinterpret_cast<char*>(&arr[0]);
        check(stride == 64, "array stride is one cache line");
        check(reinterpret_cast<uintptr_t>(&arr[0]) % 64 == 0, "array base is 64-aligned");
    }

    std::printf("\n--- checksum basics ---\n");
    {
        const bus::Message m = make(1);
        check(m.verify(), "a freshly stamped message verifies");
        check(m.compute_checksum() != 0, "checksum is not trivially zero");
    }

    std::printf("\n--- every field is covered ---\n");
    // Corrupting any field must be detected. A checksum that skips a field is
    // worse than none, because it looks like protection.
    {
        bus::Message m = make(1); m.seq ^= 1;
        check(!m.verify(), "corrupt seq detected");
    }
    { bus::Message m = make(1); m.intended_tsc ^= 1; check(!m.verify(), "corrupt intended_tsc detected"); }
    { bus::Message m = make(1); m.send_tsc     ^= 1; check(!m.verify(), "corrupt send_tsc detected"); }
    { bus::Message m = make(1); m.price        ^= 1; check(!m.verify(), "corrupt price detected"); }
    { bus::Message m = make(1); m.size         ^= 1; check(!m.verify(), "corrupt size detected"); }
    { bus::Message m = make(1); m.symbol_id    ^= 1; check(!m.verify(), "corrupt symbol_id detected"); }
    { bus::Message m = make(1); m.side         ^= 1; check(!m.verify(), "corrupt side detected"); }
    { bus::Message m = make(1); m.level        ^= 1; check(!m.verify(), "corrupt level detected"); }
    { bus::Message m = make(1); m.flags        ^= 1; check(!m.verify(), "corrupt flags detected"); }

    std::printf("\n--- order sensitivity ---\n");
    // A plain XOR fold would pass everything above and still fail this: XOR is
    // commutative, so two equal-width fields swapping values leaves it
    // unchanged. A torn message can look exactly like that.
    {
        bus::Message m = make(1);
        const uint64_t a = m.intended_tsc;
        m.intended_tsc = m.send_tsc;
        m.send_tsc = a;
        check(!m.verify(), "swapping two 8-byte fields is detected");
    }

    std::printf("\n--- torn message ---\n");
    // The real failure mode: a consumer copies a slot while the producer
    // overwrites it, so the first N bytes come from message A and the rest from
    // message B. Every individual field is a legitimate value.
    {
        const bus::Message old_msg = make(100);
        const bus::Message new_msg = make(101);
        int caught = 0, total = 0;
        for (size_t split = 8; split < 48; split += 8) {
            bus::Message torn{};
            std::memcpy(&torn, &new_msg, split);
            std::memcpy(reinterpret_cast<char*>(&torn) + split,
                        reinterpret_cast<const char*>(&old_msg) + split,
                        sizeof(bus::Message) - split);
            ++total;
            if (!torn.verify()) ++caught;
        }
        std::printf("  caught %d of %d tear points\n", caught, total);
        check(caught == total, "every tear point is detected");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
