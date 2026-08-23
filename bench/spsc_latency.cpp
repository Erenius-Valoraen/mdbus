// Cross-core latency through the lock-free SPSC ring.
//
// A producer pinned to one physical P-core fills 64-byte Messages and pushes
// them; a consumer pinned to a different physical P-core pops them, checks
// integrity, and records how long each one took to arrive.
//
// MODE: saturation. The producer runs as fast as it can rather than following
// an external arrival schedule. That measures the transport's floor, which is
// what this stage is for -- but it is NOT coordinated-omission-correct, because
// a producer that stalls simply sends late and nothing records that it was
// supposed to have sent earlier. Latency against a pre-generated intended_tsc
// schedule arrives with the load generator (DESIGN.md D8/D9).
//
// Every run prints the environment it ran in and writes it alongside the
// results, because a latency number without its conditions is not a
// measurement.

#include "bus/affinity.hpp"
#include "bus/message.hpp"
#include "bus/spsc_ring.hpp"
#include "bus/stats.hpp"
#include "bus/timing.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Ring capacity is a template parameter, so it is fixed at compile time rather
// than being a flag. Change it here; the power-of-two static_assert will catch
// a bad value.
constexpr size_t kRingSlots = 1024;

struct Config {
    uint64_t messages      = 5'000'000;
    int      producer_core = 4;
    int      consumer_core = 6;
    double   warmup_frac   = 0.10;
    std::string out_dir    = "results/spsc";
    bool     write_csv     = true;
};

void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --messages N        messages to send        (default 5000000)\n"
        "  --producer-core N   CPU for the producer    (default 4)\n"
        "  --consumer-core N   CPU for the consumer    (default 6)\n"
        "  --warmup F          fraction discarded      (default 0.10)\n"
        "  --out DIR           output directory        (default results/spsc)\n"
        "  --no-csv            skip writing samples\n"
        "  -h, --help\n", argv0);
}

bool parse(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--messages")      c.messages      = std::strtoull(next("--messages"), nullptr, 10);
        else if (a == "--producer-core") c.producer_core = std::atoi(next("--producer-core"));
        else if (a == "--consumer-core") c.consumer_core = std::atoi(next("--consumer-core"));
        else if (a == "--warmup")        c.warmup_frac   = std::atof(next("--warmup"));
        else if (a == "--out")           c.out_dir       = next("--out");
        else if (a == "--no-csv")        c.write_csv     = false;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0]); std::exit(2); }
    }
    return true;
}

using Ring = bus::SpscRing<bus::Message, kRingSlots>;

// Static rather than a local: 1024 x 64 B is 64 KB, which is fine on the stack
// but there is no reason to put it there, and a fixed address keeps the slot
// alignment obvious.
Ring ring;

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse(argc, argv, cfg)) return 0;

    const bus::TscClock clk = bus::calibrate_tsc(100);

    std::printf("environment\n");
    std::printf("  tsc            %.3f MHz  (calibrated against CLOCK_MONOTONIC)\n", clk.tsc_freq_mhz());
    std::printf("  invariant tsc  %s\n", clk.invariant   ? "yes" : "NO -- results are not trustworthy");
    std::printf("  hypervisor     %s\n", clk.virtualized ? "yes -- host may preempt the vCPU" : "no");
    std::printf("  cursors        %s\n", Ring::padded ? "padded onto separate cache lines" : "PACKED (BUS_NO_PADDING)");
    std::printf("\nconfiguration\n");
    std::printf("  producer core  %d\n", cfg.producer_core);
    std::printf("  consumer core  %d\n", cfg.consumer_core);
    std::printf("  ring slots     %zu  (%zu B message, %zu KB total)\n",
                kRingSlots, sizeof(bus::Message), kRingSlots * sizeof(bus::Message) / 1024);
    std::printf("  messages       %llu\n", (unsigned long long)cfg.messages);
    std::printf("  warmup         %.0f%% discarded\n\n", cfg.warmup_frac * 100.0);

    std::vector<uint64_t> latency(cfg.messages);
    std::atomic<uint64_t> gaps{0}, torn{0};
    std::atomic<bool> pin_failed{false};

    const uint64_t t_start = bus::rdtsc_relaxed();

    std::thread producer([&] {
        if (!bus::pin_and_verify(cfg.producer_core)) { pin_failed.store(true); return; }
        for (uint64_t seq = 0; seq < cfg.messages; ++seq) {
            bus::Message m{};
            m.seq       = seq;
            m.price     = 4250000000 + static_cast<int64_t>(seq % 1000);
            m.size      = 100;
            m.symbol_id = 7;
            m.side      = static_cast<uint8_t>(bus::Side::Bid);
            m.level     = 1;
            m.flags     = static_cast<uint16_t>(bus::UpdateType::Add);

            // Stamped last so the interval measures transport, not the cost of
            // building the payload.
            m.send_tsc = bus::rdtsc_relaxed();
            m.stamp();

            while (!ring.try_push(m)) {}   // spin while full: consumer is behind
        }
    });

    std::thread consumer([&] {
        if (!bus::pin_and_verify(cfg.consumer_core)) { pin_failed.store(true); return; }
        bus::Message m{};
        for (uint64_t expected = 0; expected < cfg.messages; ++expected) {
            while (!ring.try_pop(m)) {}    // spin while empty: producer is behind
            const uint64_t recv = bus::rdtsc_relaxed();

            if (m.seq != expected) gaps.fetch_add(1, std::memory_order_relaxed);
            if (!m.verify())       torn.fetch_add(1, std::memory_order_relaxed);

            latency[expected] = recv - m.send_tsc;
        }
    });

    producer.join();
    consumer.join();

    if (pin_failed.load()) {
        std::fprintf(stderr, "could not pin to the requested cores -- aborting\n");
        return 1;
    }

    const double secs = clk.to_ns(bus::rdtsc_relaxed() - t_start) / 1e9;

    std::printf("integrity\n");
    std::printf("  gaps / dups / reorders  %llu\n", (unsigned long long)gaps.load());
    std::printf("  torn messages           %llu\n\n", (unsigned long long)torn.load());

    const size_t skip = static_cast<size_t>(cfg.warmup_frac * static_cast<double>(cfg.messages));
    std::vector<uint64_t> kept(latency.begin() + static_cast<long>(skip), latency.end());

    // CSV first: summarise() sorts in place, and the plots need arrival order
    // for the time-series panel.
    bool csv_ok = true;
    const std::string csv = cfg.out_dir + "/latency.csv";
    if (cfg.write_csv) csv_ok = bus::write_samples_csv(csv, kept, clk);

    const bus::Stats s = bus::summarise(kept, clk);   // sorts `kept` in place
    bus::print_stats(s, "one-way latency, producer core -> consumer core");

    const double mps = static_cast<double>(cfg.messages) / secs / 1e6;
    std::printf("\nthroughput (saturation -- producer unpaced)\n");
    std::printf("    %.2f M msg/s   %.0f MB/s   over %.3f s\n", mps,
                static_cast<double>(cfg.messages) * sizeof(bus::Message) / secs / 1e6, secs);

    if (cfg.write_csv) {
        if (!csv_ok) {
            std::fprintf(stderr, "could not write %s\n", csv.c_str());
            return 1;
        }
        const std::string meta = cfg.out_dir + "/run_meta.json";
        std::FILE* f = std::fopen(meta.c_str(), "w");
        if (f) {
            std::fprintf(f,
                "{\n"
                "  \"transport\": \"spsc_ring\",\n"
                "  \"mode\": \"saturation\",\n"
                "  \"messages\": %llu,\n"
                "  \"message_bytes\": %zu,\n"
                "  \"ring_slots\": %zu,\n"
                "  \"producer_core\": %d,\n"
                "  \"consumer_core\": %d,\n"
                "  \"cursors_padded\": %s,\n"
                "  \"warmup_fraction\": %.3f,\n"
                "  \"tsc_mhz\": %.3f,\n"
                "  \"invariant_tsc\": %s,\n"
                "  \"virtualized\": %s,\n"
                "  \"gaps\": %llu,\n"
                "  \"torn\": %llu,\n"
                "  \"samples\": %zu,\n"
                "  \"min_ns\": %.1f, \"mean_ns\": %.1f, \"p50_ns\": %.1f, \"p90_ns\": %.1f,\n"
                "  \"p99_ns\": %.1f, \"p999_ns\": %.1f, \"p9999_ns\": %.1f, \"max_ns\": %.1f,\n"
                "  \"throughput_mmsg_s\": %.3f, \"elapsed_s\": %.3f\n"
                "}\n",
                (unsigned long long)cfg.messages, sizeof(bus::Message), kRingSlots,
                cfg.producer_core, cfg.consumer_core, Ring::padded ? "true" : "false",
                cfg.warmup_frac, clk.tsc_freq_mhz(),
                clk.invariant ? "true" : "false", clk.virtualized ? "true" : "false",
                (unsigned long long)gaps.load(), (unsigned long long)torn.load(),
                s.count, s.min_ns, s.mean_ns, s.p50_ns, s.p90_ns,
                s.p99_ns, s.p999_ns, s.p9999_ns, s.max_ns, mps, secs);
            std::fclose(f);
        }
        std::printf("\nwrote %s  (%zu samples)\n      %s\n", csv.c_str(), s.count, meta.c_str());
    }

    return (gaps.load() == 0 && torn.load() == 0) ? 0 : 1;
}
