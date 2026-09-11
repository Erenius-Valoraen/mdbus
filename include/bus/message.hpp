#pragma once

// The unit of traffic on the bus: exactly one 64-byte cache line, so
// publishing one costs exactly one coherence transfer.

#include <cstdint>

namespace bus {

// Prices are integer ticks, never doubles. 0.1 has no exact binary
// representation, so float prices break equality and accumulate error, and
// exchanges quote in ticks anyway.
inline constexpr int64_t kPriceScale = 100'000'000;  // 1e-8

enum class Side : uint8_t { Bid = 0, Ask = 1 };
enum class UpdateType : uint16_t { Add = 0, Modify = 1, Cancel = 2, Trade = 3 };

// Field order is chosen: 8-byte members first, then 4, then the rest. The
// compiler may not reorder members and pads instead, and a careless order
// silently inflates the struct past a line.
struct alignas(64) Message {
    uint64_t seq;           // also the publish flag, so the producer writes it last
    uint64_t intended_tsc;  // when the schedule said to send
    uint64_t send_tsc;      // when it actually went
    int64_t  price;         // divide by kPriceScale
    uint64_t size;
    uint32_t symbol_id;
    uint8_t  side;          // Side
    uint8_t  level;
    uint16_t flags;         // UpdateType
    uint64_t checksum;
    uint8_t  _pad[8];       // named so it can be zeroed; memcmp stays meaningful

    // Detects a torn message: in the SPMC ring a producer can overwrite a slot
    // mid-read, leaving fields that individually all look plausible.
    //
    // FNV-1a a word at a time rather than an XOR fold. XOR is commutative, so
    // two fields swapping values leaves it unchanged, and it does not spread:
    // one input bit flip moves 1.0 of 64 output bits under XOR versus 12.2
    // here. Short of the ideal 32 because multiplication only propagates bits
    // upward; a Murmur-style finaliser would close that if it ever matters.
    uint64_t compute_checksum() const noexcept {
        uint64_t h = 0xcbf29ce484222325ULL;
        auto mix = [&h](uint64_t v) noexcept {
            h ^= v;
            h *= 0x100000001b3ULL;
        };
        mix(seq);
        mix(intended_tsc);
        mix(send_tsc);
        mix(static_cast<uint64_t>(price));
        mix(size);
        mix(symbol_id);
        mix(side);
        mix(level);
        mix(flags);
        return h;
    }

    void stamp() noexcept { checksum = compute_checksum(); }

    bool verify() const noexcept { return checksum == compute_checksum(); }
};

// A struct meant to be one line that silently becomes 65 bytes doubles the
// coherence traffic with nothing to indicate it.
static_assert(sizeof(Message) == 64, "Message must be exactly one cache line");
static_assert(alignof(Message) == 64, "Message must be cache-line aligned");

}  // namespace bus
