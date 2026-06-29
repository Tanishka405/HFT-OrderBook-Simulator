#include "memory_pool.hpp"
#include "order_types.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <thread>

using namespace hft;

void test_basic_alloc_dealloc() {
    MemoryPool<Order, 128> pool;
    assert(pool.capacity()  == 128);
    assert(pool.available() == 128);
    assert(pool.in_use()    == 0);

    Order* o = pool.allocate();
    assert(o != nullptr);
    assert(pool.in_use()    == 1);
    assert(pool.available() == 127);

    pool.deallocate(o);
    assert(pool.in_use()    == 0);
    assert(pool.available() == 128);
    std::cout << "  [PASS] basic_alloc_dealloc\n";
}

void test_pool_exhaustion() {
    constexpr std::size_t CAP = 32;
    MemoryPool<Order, CAP> pool;

    std::vector<Order*> ptrs;
    ptrs.reserve(CAP);

    for (std::size_t i = 0; i < CAP; ++i) {
        Order* o = pool.allocate();
        assert(o != nullptr);
        ptrs.push_back(o);
    }

    assert(pool.available() == 0);
    assert(pool.allocate()  == nullptr);  // must return nullptr on exhaustion

    for (Order* p : ptrs) pool.deallocate(p);
    assert(pool.available() == CAP);
    std::cout << "  [PASS] pool_exhaustion\n";
}

void test_cache_line_alignment() {
    MemoryPool<Order, 16> pool;
    Order* a = pool.allocate();
    Order* b = pool.allocate();

    // Each slot is padded to a multiple of 64 bytes, so consecutive
    // allocations should differ by at least 64 bytes.
    uintptr_t diff = std::abs(
        static_cast<ptrdiff_t>(reinterpret_cast<uintptr_t>(b)) -
        static_cast<ptrdiff_t>(reinterpret_cast<uintptr_t>(a)));
    assert(diff >= 64);
    assert(reinterpret_cast<uintptr_t>(a) % 64 == 0);

    pool.deallocate(a);
    pool.deallocate(b);
    std::cout << "  [PASS] cache_line_alignment (diff=" << diff << " bytes)\n";
}

void test_reuse_after_dealloc() {
    MemoryPool<Order, 4> pool;
    Order* o1 = pool.allocate();
    o1->id = 99;
    pool.deallocate(o1);

    Order* o2 = pool.allocate();
    assert(o2 != nullptr);
    o2->id = 42;  // should not crash
    pool.deallocate(o2);
    std::cout << "  [PASS] reuse_after_dealloc\n";
}

int main() {
    std::cout << "=== MemoryPool Unit Tests ===\n";
    test_basic_alloc_dealloc();
    test_pool_exhaustion();
    test_cache_line_alignment();
    test_reuse_after_dealloc();
    std::cout << "All MemoryPool tests passed.\n";
    return 0;
}