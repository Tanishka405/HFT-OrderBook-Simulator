#pragma once

#include "order_types.hpp"
#include "spsc_ring_buffer.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <string>

namespace hft {

/**
 * @brief Asynchronous market data feed using an SPSC ring buffer.
 *
 * The publishing thread (producer) calls publish() on each price update.
 * A dedicated consumer thread drains the ring buffer and dispatches ticks
 * to registered handlers — simulating a real exchange multicast feed.
 *
 * Latency profile:
 *  - publish():  ~10–30 ns (ring buffer push + release fence)
 *  - consume():  ~20–50 ns end-to-end (including handler dispatch)
 */
class MarketDataFeed {
public:
    static constexpr std::size_t RING_SIZE = 8192;  // must be power of two

    using TickHandler = std::function<void(const MarketDataTick&)>;

    explicit MarketDataFeed(std::string feed_name);
    ~MarketDataFeed();

    // Non-copyable
    MarketDataFeed(const MarketDataFeed&)            = delete;
    MarketDataFeed& operator=(const MarketDataFeed&) = delete;

    /**
     * @brief Register a callback invoked for every tick on the consumer thread.
     *        Must be called before start().
     */
    void register_handler(TickHandler handler);

    /**
     * @brief Start the consumer thread.
     */
    void start();

    /**
     * @brief Stop the consumer thread and flush remaining ticks.
     */
    void stop();

    /**
     * @brief Publish a market data tick (called from the producer/matching thread).
     * @return true if the tick was enqueued; false if the ring was full.
     */
    bool publish(const MarketDataTick& tick) noexcept;

    [[nodiscard]] uint64_t ticks_published()  const noexcept;
    [[nodiscard]] uint64_t ticks_consumed()   const noexcept;
    [[nodiscard]] uint64_t ticks_dropped()    const noexcept;
    [[nodiscard]] const std::string& name()   const noexcept { return name_; }

private:
    void consumer_loop();

    std::string   name_;
    SPSCRingBuffer<MarketDataTick, RING_SIZE> ring_;

    std::thread        consumer_thread_;
    std::atomic<bool>  running_{false};
    TickHandler        handler_;

    alignas(64) std::atomic<uint64_t> published_{0};
    alignas(64) std::atomic<uint64_t> consumed_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

} // namespace hft