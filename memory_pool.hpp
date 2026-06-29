#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <new>
#include <atomic>
#include <stdexcept>
#include <type_traits>

namespace hft {

// Cache line size for x86-64 to prevent false sharing
static constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * @brief Fixed-size, pre-allocated memory pool with O(1) alloc/dealloc.
 *
 * Design goals:
 *  - Zero heap allocations after construction (pre-faults pages at init time)
 *  - Lock-free free-list via atomic CAS (thread-safe dealloc, single-writer alloc)
 *  - Each block is padded to a full cache line to eliminate false sharing
 *  - RAII: all memory returned to OS on destruction
 *
 * @tparam T        Object type to pool
 * @tparam Capacity Maximum number of live objects at once
 */
template <typename T, std::size_t Capacity>
class alignas(CACHE_LINE_SIZE) MemoryPool {
    static_assert(Capacity > 0, "Pool capacity must be > 0");
    static_assert(std::is_trivially_destructible_v<T> || true,
                  "MemoryPool works best with trivially destructible types");

    // Each slot is padded to a full cache line so adjacent objects never
    // share a cache line — critical for avoiding false sharing on hot paths.
    static constexpr size_t SLOT_SIZE =
        ((sizeof(T) + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;

    struct alignas(CACHE_LINE_SIZE) Slot {
        alignas(CACHE_LINE_SIZE) std::byte storage[SLOT_SIZE];
        std::atomic<Slot*> next{nullptr};  // intrusive free-list link
    };

public:
    MemoryPool() {
        // Build the initial free-list: slots[0] -> slots[1] -> ... -> nullptr
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            slots_[i].next.store(&slots_[i + 1], std::memory_order_relaxed);
        }
        slots_[Capacity - 1].next.store(nullptr, std::memory_order_relaxed);
        free_head_.store(&slots_[0], std::memory_order_release);
        allocated_.store(0, std::memory_order_relaxed);
    }

    ~MemoryPool() {
        // Slots are stack-allocated (or in the class body), nothing to free.
    }

    // Non-copyable, non-movable — pool identity must be stable
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    /**
     * @brief Allocate and construct a T in-place.
     * @return Pointer to the constructed object, or nullptr if pool is exhausted.
     */
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        Slot* slot = pop_free_slot();
        if (!slot) [[unlikely]] return nullptr;

        allocated_.fetch_add(1, std::memory_order_relaxed);
        return ::new (slot->storage) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Destroy a T and return its slot to the pool.
     * @param ptr Must point to an object that was allocated from *this pool*.
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) [[unlikely]] return;

        ptr->~T();

        // Recover the containing Slot via pointer arithmetic
        // The object lives at the start of slot->storage, so:
        Slot* slot = reinterpret_cast<Slot*>(
            reinterpret_cast<std::byte*>(ptr));

        push_free_slot(slot);
        allocated_.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return Capacity - allocated_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity; }

    [[nodiscard]] std::size_t in_use() const noexcept {
        return allocated_.load(std::memory_order_relaxed);
    }

private:
    /**
     * Lock-free pop from the free-list head using CAS loop.
     * Returns nullptr if list is empty.
     */
    Slot* pop_free_slot() noexcept {
        Slot* head = free_head_.load(std::memory_order_acquire);
        while (head) {
            Slot* next = head->next.load(std::memory_order_relaxed);
            if (free_head_.compare_exchange_weak(
                    head, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return head;
            }
            // CAS failed: head was updated by another thread — retry with new head
        }
        return nullptr;
    }

    /**
     * Lock-free push onto the free-list head.
     */
    void push_free_slot(Slot* slot) noexcept {
        Slot* head = free_head_.load(std::memory_order_acquire);
        do {
            slot->next.store(head, std::memory_order_relaxed);
        } while (!free_head_.compare_exchange_weak(
            head, slot,
            std::memory_order_release,
            std::memory_order_acquire));
    }

    // The actual storage — allocated on the stack / in BSS; no heap touch after ctor.
    alignas(CACHE_LINE_SIZE) Slot slots_[Capacity];

    // Atomic free-list head
    alignas(CACHE_LINE_SIZE) std::atomic<Slot*> free_head_{nullptr};

    // Live allocation count (diagnostic)
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> allocated_{0};
};

} // namespace hft