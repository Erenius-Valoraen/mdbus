#pragma once

// Percentiles and result output. Nothing here runs while anything is being
// measured: samples are collected as raw ticks and converted afterwards.

#include "bus/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace bus {

struct Stats {
    size_t count    = 0;
    double min_ns   = 0;
    double mean_ns  = 0;
    double p50_ns   = 0;
    double p90_ns   = 0;
    double p99_ns   = 0;
    double p999_ns  = 0;
    double p9999_ns = 0;
    double max_ns   = 0;
};

// Sorts in place. Nearest-rank percentiles, so a reported p99.9 is a value that
// was actually observed rather than interpolated between two neighbours.
inline Stats summarise(std::vector<uint64_t>& ticks, const TscClock& clk) {
    Stats s;
    if (ticks.empty()) return s;

    std::sort(ticks.begin(), ticks.end());
    s.count = ticks.size();

    const auto q = [&](double p) {
        const size_t i = static_cast<size_t>(p * static_cast<double>(ticks.size() - 1));
        return clk.to_ns(ticks[i]);
    };

    long double total = 0;
    for (uint64_t t : ticks) total += static_cast<long double>(t);

    s.min_ns   = clk.to_ns(ticks.front());
    s.max_ns   = clk.to_ns(ticks.back());
    s.mean_ns  = static_cast<double>(total / static_cast<long double>(ticks.size())) * clk.ns_per_tick;
    s.p50_ns   = q(0.50);
    s.p90_ns   = q(0.90);
    s.p99_ns   = q(0.99);
    s.p999_ns  = q(0.999);
    s.p9999_ns = q(0.9999);
    return s;
}

inline void print_stats(const Stats& s, const char* label) {
    std::printf("%s  (n = %zu)\n", label, s.count);
    std::printf("    min    %9.1f ns\n", s.min_ns);
    std::printf("    mean   %9.1f ns\n", s.mean_ns);
    std::printf("    p50    %9.1f ns\n", s.p50_ns);
    std::printf("    p90    %9.1f ns\n", s.p90_ns);
    std::printf("    p99    %9.1f ns\n", s.p99_ns);
    std::printf("    p99.9  %9.1f ns\n", s.p999_ns);
    std::printf("    p99.99 %9.1f ns\n", s.p9999_ns);
    std::printf("    max    %9.1f ns\n", s.max_ns);
}

// Call before summarise(), which sorts in place: the time-series panel needs
// arrival order.
inline bool write_samples_csv(const std::string& path,
                              const std::vector<uint64_t>& ticks,
                              const TscClock& clk,
                              int consumer_id = 0) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "consumer_id,sample_ns\n");
    for (uint64_t t : ticks) std::fprintf(f, "%d,%.1f\n", consumer_id, clk.to_ns(t));
    std::fclose(f);
    return true;
}

}  // namespace bus
