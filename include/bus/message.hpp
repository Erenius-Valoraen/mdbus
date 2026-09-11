#pragma once

// The unit of traffic on the bus.
//
// Sized and aligned to exactly one 64-byte cache line, because publishing a
// message then costs exactly one coherence transfer. A message spanning two
// lines costs two -- which is the whole content of experiment E7, so the 64-byte
// case has to actually be 64 bytes for the comparison to mean anything.
//
// Field order is chosen, not incidental: 8-byte members first, then 4, then the
// small ones. The compiler may not reorder members, so it pads instead, and a
// careless order silently inflates the struct past a cache line.

#include <cstdint>

namespace bus {

// Ticks per price unit. Prices are integers, never doubles: 0.1 has no exact
// binary representation, so float prices break equality comparison and
// accumulate error, and exchanges quote in integer ticks to begin with.
inline constexpr int64_t kPriceScale = 100'000'000;  // 1e-8

enum class Side : uint8_t { Bid = 0, Ask = 1 };

enum class UpdateType : uint16_t { Add = 0, Modify = 1, Cancel = 2, Trade = 3 };

struct alignas(64) Message {
    // seq doubles as the publish flag: a consumer treats a slot as ready when
    // the seq it finds there matches the seq it wants, so the producer must
    // write this LAST. That ordering is the ring's responsibility, not the
    // struct's -- position in the declaration says nothing about write order.
    uint64_t seq;

    uint64_t intended_tsc;  // when the schedule said to send this
    uint64_t send_tsc;      // when it was actually sent
    int64_t  price;         // fixed point; divide by kPriceScale
    uint64_t size;          // quantity
    uint32_t symbol_id;
    uint8_t  side;          // Side
    uint8_t  level;         // book level
    uint16_t flags;         // UpdateType
    uint64_t checksum;
    uint8_t _pad[8];

    // The fields above total 48 bytes, leaving 16 before the cache line is
    // full.

    // Compute a checksum over every OTHER field. Must not include the checksum
    // field itself, or verification can never succeed.
    //
    // Its job is detecting a *torn* message: in the SPMC ring the producer may
    // overwrite a slot while a consumer is mid-read, so the consumer sees some
    // fields from the old message and some from the new one. Individually the
    // values look plausible; only a whole-message check catches it.
    //
    // A plain XOR of the 8-byte fields is a poor choice here -- XOR is
    // order-insensitive, so two fields swapping values leaves it unchanged.
    // Mix instead: fold each field in with a multiply by a large odd constant
    // and a shift/xor, so every input bit can affect every output bit.
    uint64_t compute_checksum() const noexcept {
        uint64_t h = 0xcbf29ce484222325ULL;
        auto mix = [&h](uint64_t v) noexcept {
            h ^= v;
            h *=  0x100000001b3ULL;
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

    void stamp() noexcept {
        checksum = compute_checksum();
    }

    bool verify() const noexcept {
        return checksum == compute_checksum();
    }
};

// A struct meant to be one cache line that silently becomes 65 bytes doubles
// the coherence traffic per message with nothing to indicate it. Checked at
// compile time so adding a field later fails the build rather than the tail.
static_assert(sizeof(Message) == 64, "Message must be exactly one cache line");
static_assert(alignof(Message) == 64, "Message must be cache-line aligned");

}  // namespace bus
