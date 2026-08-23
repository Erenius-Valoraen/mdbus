#include <vector>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <iostream>

#include "bus/timing.hpp"
#include "bus/affinity.hpp"
#include "bus/message.hpp"



class Market {
    private:
        int core = 4; // according to my computer

    public:
        Market();
        bus::TscClock clock;
        bus::Message send_message();
        void run_producer(std::atomic<bool> &keep_running, std::vector<bus::Message> & shared_ring);

};

Market::Market() {
    clock = bus::calibrate_tsc(100);
}

bus::Message Market::send_message() {
    bus::Message m;
    
    m = {};
    
    m.seq = 0;
    m.intended_tsc = bus::rdtsc_relaxed();
    m.price = 10000;
    m.size = 100;
    m.symbol_id = 1234;
    m.side = 1;
    m.level = 1;
    m.flags = 1;

    m.send_tsc = bus::rdtsc_relaxed();
    m.stamp();

    return m;
}

void Market::run_producer
(std::atomic<bool> & keep_running, std::vector<bus::Message> & shared_ring) {

    bus::pin_and_verify(this->core);

    uint64_t sequence = 0;
    size_t ring_size = shared_ring.size();

    while (keep_running.load(std::memory_order_relaxed)) {
        size_t slot = sequence % ring_size; // makes the sequence wrap around

        bus::Message m = send_message();
        m.seq = sequence;

        m.send_tsc = bus::rdtsc_relaxed();
        m.stamp();

        shared_ring[slot] = m;
        
        sequence++; // advances the sequence


    }

}


class Receiver {
    private:
        int core = 6;

    public:
        void run_consumer
        (std::atomic<bool> & keep_running, std::vector<bus::Message> & shared_ring);


};

void Receiver::run_consumer
(std::atomic<bool> & keep_running, std::vector<bus::Message> & shared_ring) {
    bus::pin_and_verify(this->core);

    uint64_t next_expected_seq = 0;
    size_t ring_size = shared_ring.size();

    while (keep_running.load(std::memory_order_relaxed)) {
        size_t slot = next_expected_seq % ring_size;

        const bus::Message & shared_message = shared_ring[slot];

        if (shared_message.seq == next_expected_seq) {
            // do something, maybe a trading strategy
            trading_strategy(shared_message);

        }

    }
}

void trading_strategy(bus::Message m) {
    uint64_t t0 = bus::rdtsc_relaxed();
    if (m.seq <= 3) {
        std::cout << m.price  << "\n";
        std::cout << m.checksum << "\n";
    }
    uint64_t t1 = bus::rdtsc_relaxed();
    std::cout << "Time taken for trading decision: " << t1 - 10 << "\n";
}

int main() {
    std::atomic<bool> keep_running{true};
    std::vector<bus::Message> shared_bus_ring(1024, bus::Message{});
    Market market;
    Receiver receiver;

    std::thread producer_thread(
        &Market::run_producer,
        &market,
        std::ref(keep_running),
        std::ref(shared_bus_ring)
    );

    std::thread consumer_thread(
        &Receiver::run_consumer,
        &receiver,
        std::ref(keep_running),
        std::ref(shared_bus_ring)
    );

    std::this_thread::sleep_for(std::chrono::seconds(1));
    keep_running.store(false);

    producer_thread.join();
    consumer_thread.join();

    // for (bus::Message message : shared_bus_ring) {
    //     std::cout << message.price << ", ";
    // }


}