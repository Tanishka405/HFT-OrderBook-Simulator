#include "spsc_ring_buffer.hpp"
#include "order_types.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

using namespace hft;

void test_basic_push_pop() {
    SPSCRingBuffer<int, 8> ring;
    assert(ring.empty());
    assert(!ring.full());

    assert(ring.push(42));
    assert(!ring.empty());

    auto val = ring.pop();
    assert(val.has_value());
    assert(*val == 42);
    assert(ring.empty());
    std::cout << "  [PASS] basic_push_pop\n";
}

void test_capacity_boundary() {
    // Capacity-1 usable slots (one kept empty for empty/full distinction)
    SPSCRingBuffer<int, 4> ring;
    assert(ring.push(1));
    assert(ring.push(2));
    assert(ring.push(3));
    assert(!ring.push(4));  // full — must fail

    auto v1 = ring.pop(); assert(v1 && *v1 == 1);
    auto v2 = ring.pop(); assert(v2 && *v2 == 2);
    auto v3 = ring.pop(); assert(v3 && *v3 == 3);
    assert(!ring.pop().has_value());  // empty
    std::cout << "  [PASS] capacity_boundary\n";
}

void test_fifo_ordering() {
    SPSCRingBuffer<int, 16> ring;
    for (int i = 0; i < 10; ++i) ring.push(i);
    for (int i = 0; i < 10; ++i) {
        auto v = ring.pop();
        assert(v.has_value() && *v == i);
    }
    std::cout << "  [PASS] fifo_ordering\n";
}

void test_wrap_around() {
    SPSCRingBuffer<int, 8> ring;
    // Fill and drain multiple times to force index wrap-around
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 7; ++i) assert(ring.push(i));
        for (int i = 0; i < 7; ++i) {
            auto v = ring.pop();
            assert(v.has_value() && *v == i);
        }
    }
    std::cout << "  [PASS] wrap_around\n";
}

void test_concurrent_spsc() {
    constexpr std::size_t N = 500'000;
    SPSCRingBuffer<uint64_t, 1024> ring;
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i) {
            while (!ring.push(i)) std::this_thread::yield();
        }
    });

    uint64_t expected = 0;
    std::thread consumer([&] {
        while (expected < N) {
            auto v = ring.pop();
            if (v) {
                assert(*v == expected++);
            }
        }
        done.store(true);
    });

    producer.join();
    consumer.join();

    assert(done.load());
    assert(expected == N);
    std::cout << "  [PASS] concurrent_spsc (" << N << " messages)\n";
}

int main() {
    std::cout << "=== SPSCRingBuffer Unit Tests ===\n";
    test_basic_push_pop();
    test_capacity_boundary();
    test_fifo_ordering();
    test_wrap_around();
    test_concurrent_spsc();
    std::cout << "All SPSCRingBuffer tests passed.\n";
    return 0;
}