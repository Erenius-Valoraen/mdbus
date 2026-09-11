// Two threads bounce a value between two cores through a four-slot ring, and
// one of them times the round trip. This is the smallest thing that measures
// how long a cache line takes to move between physical cores.
//
//   ./pingpong_legacy samples.csv
//   python plots/plot_run.py samples.csv

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <fstream>

#include "bus/affinity.hpp"
#include "bus/timing.hpp"

template <size_t N>
std::ostream& operator<<(std::ostream& os, const std::atomic<int>(&arr)[N]) {
    os << "[";
    for (size_t i = 0; i < N; ++i) {
        // Safe to use standard load() or implicit conversion here
        os << arr[i].load();
        if (i < N - 1) os << ", ";
    }
    os << "]";
    return os;
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& vec) {
    os << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        os << vec[i];
        if (i != vec.size() - 1) {
            os << ", "; // Add comma between elements, but not at the end
        }
    }
    os << "]";
    return os;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <output.csv>\n";
        return 2;
    }
    // Two distinct physical performance cores. Siblings of one core would share
    // L1 and L2, so the message would never actually cross a core.
    constexpr int kCoreA = 4;
    constexpr int kCoreB = 6;

    const int ring_length = 4;
    std::atomic<int> ring1[ring_length] = {0, 0, 0, 0};
    std::atomic<int> ring2[ring_length] = {0, 0, 0, 0};
    const int N = 1000000;
    std::vector<uint64_t> samples(N);


    std::thread a([&] {
        if (!bus::pin_and_verify(kCoreA)) {
            std::cerr << "could not pin to core " << kCoreA << "\n";
            return;
        }
        int i = 0;
        int HOPS = 0;
        int val = 1;
        uint32_t cpu_id = 0;
        while (HOPS < N) {
            uint64_t start_tick = bus::rdtsc_ordered(cpu_id);
            
            ring1[i].store(val, std::memory_order_release); // send the ping
            while (ring2[i].load() != val + 1) {} // wait for the pong

            uint64_t end_tick = bus::rdtsc_ordered(cpu_id);

            samples[HOPS] = end_tick - start_tick;

            val = ring2[i].load(std::memory_order_acquire) + 1;  // Update the value according to the received pong
            i = (i + 1) % ring_length; // get the next index or wrap around
            HOPS++;
            // next iteration automatically sends the ping again          
            
        }

    });
    

    std::thread b([&] {
        if (!bus::pin_and_verify(kCoreB)) {
            std::cerr << "could not pin to core " << kCoreB << "\n";
            return;
        }
        int i = 0;
        int HOPS = 0;

        while (HOPS < N) {
            while (ring1[i].load() <= ring2[i].load()) {} // wait for the ping
            int val = ring1[i].load(); // get the value

            ring2[i].store(val + 1); // send the pong by updating the value
            i = (i + 1) % ring_length; // update the index
            HOPS++;
        }
   
    });

    a.join();
    b.join();

    bus::TscClock ns_clock = bus::calibrate_tsc(100);
    std::vector<double> ns_times(N);

    for (int i = 0; i < N; i++) {
        ns_times[i] = ns_clock.to_ns(samples[i]);
    }

    std::cout << ring1 << "\n";
    std::cout << ring2 << "\n";

    std::ofstream out(argv[1]);
    if (!out) {
        std::cerr << "cannot write " << argv[1] << "\n";
        return 1;
    }
    for (double s : ns_times) out << s << "\n";
    std::cout << "wrote " << N << " samples to " << argv[1] << "\n";
    return 0;
}