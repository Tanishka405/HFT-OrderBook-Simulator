#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <cstddef>
#include <type_traits>

namespace hft {

static constexpr std::size_t CACHE_LINE_BYTES = 64;

/**
 * @brief Single-Producer / Single-Consumer (SPSC) lock-free ring buffer.
 *
 * Correctness guarantees:
 *  - Exactly one thread calls push(); exactly one (different) thread calls pop().
 *  - No mutexes; synchronisation is achieved via acquire/release fences on the
 *    head/tail indices, which carry a happens-before edge for the slot payload.
 *  - The producer and consumer indices are placed on separate cache lines to
 *    prevent false sharing (a very common performance bug in ring buffers).
 *
 * Capacity:
 *  - Must be a power of two so that index masking replaces modulo.
 *  - Usable slots = Capacity - 1  (one slot is always left empty to distinguish
 *    full from empty without a separate counter).
 *
 * @tparam T        Value type (should be cheaply copyable; trivially destructible ideal)
 * @tparam Capacity Ring size — MUST be a power of two (checked at compile time)
 */
template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SPSCRingBuffer: Capacity must be a power of two");
    static_assert(Capacity >= 2, "SPSCRingBuffer: Capacity must be >= 2");

    static constexpr std::size_t MASK = Capacity - 1;

public:
    SPSCRingBuffer() noexcept
        : head_(0), tail_(0) {}

    // Non-copyable; moving a live buffer would invalidate the producer/consumer
    // split invariant, so we forbid both.
    SPSCRingBuffer(const SPSCRingBuffer&)            = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&)                 = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&)      = delete;

    /**
     * @brief Try to enqueue one item. Called by the PRODUCER thread only.
     *
     * Uses a relaxed load of head_ (cached in the producer) and an
     * acquire load of tail_ to detect fullness without stalling the consumer.
     *
     * @return true if the item was enqueued; false if the buffer was full.
     */
    bool push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & MASK;

        // Buffer is full when advancing head would collide with tail
        if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }

        buffer_[h] = item;

        // Release store: everything written to buffer_[h] is visible before
        // the consumer sees the updated head index.
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    /**
     * @brief Move-push overload (zero copy for move-only types).
     */
    bool push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & MASK;

        if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }

        buffer_[h] = std::move(item);
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    /**
     * @brief Try to dequeue one item. Called by the CONSUMER thread only.
     *
     * Acquire load of head_ creates the happens-before edge: the consumer
     * sees the write to buffer_[t] performed by the producer before its
     * release store of head_.
     *
     * @return The item if the buffer was non-empty, std::nullopt otherwise.
     */
    std::optional<T> pop() noexcept(std::is_nothrow_copy_constructible_v<T>) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);

        // Acquire load: synchronises-with the release store in push().
        if (t == head_.load(std::memory_order_acquire)) [[unlikely]] {
            return std::nullopt;  // empty
        }

        T item = buffer_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    /**
     * @brief Peek at the front item without consuming it. Consumer thread only.
     */
    [[nodiscard]] const T* peek() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return nullptr;
        return &buffer_[t];
    }

    /**
     * @brief Approximate size (may be stale by 1 across threads).
     */
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t + Capacity) & MASK;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::size_t h    = head_.load(std::memory_order_acquire);
        const std::size_t next = (h + 1) & MASK;
        return next == tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    // Separate producer and consumer indices onto different cache lines.
    // Without this padding a write to tail_ (consumer) would dirty the cache
    // line containing head_ (producer), causing a full round-trip to coherency
    // traffic — easily 100+ ns per operation.
    alignas(CACHE_LINE_BYTES) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE_BYTES) std::atomic<std::size_t> tail_;

    // Ring storage — kept on its own cache line boundary.
    alignas(CACHE_LINE_BYTES) std::array<T, Capacity> buffer_;
};

} // namespace hft