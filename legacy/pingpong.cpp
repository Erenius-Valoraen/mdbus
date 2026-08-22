#include <iostream>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <vector>
#include <chrono>
#include <fstream>

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


void pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int main() {
    const int ring_length = 4;
    std::atomic<int> ring1[ring_length] = {0, 0, 0, 0};
    std::atomic<int> ring2[ring_length] = {0, 0, 0, 0};
    const int N = 1000000;
    std::vector<double> samples(N);


    std::thread a([&] {
        pin_to_core(2);
        int i = 0;
        int HOPS = 0;
        int val = 1;
        while (HOPS < N) {
            auto t1 = std::chrono::steady_clock::now();

            ring1[i].store(val); // send the ping
            while (ring2[i].load() != val + 1) {} // wait for the pong

            auto t2 = std::chrono::steady_clock::now();

            samples[HOPS] = std::chrono::duration<double, std::nano>(t2 - t1).count();

            val = ring2[i].load() + 1;  // Update the value according to the received pong
            i = (i + 1) % ring_length; // get the next index or wrap around
            HOPS++;
            // next iteration automatically sends the ping again          
            
        }

    });
    

    std::thread b([&] {
        pin_to_core(4);
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

    std::cout << ring1 << "\n";
    std::cout << ring2 << "\n";

    std::ofstream out("samples.csv");
    for (double s : samples) out << s << "\n";
}