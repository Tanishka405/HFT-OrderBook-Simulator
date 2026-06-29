#include "market_data_feed.hpp"
#include <thread>
#include <chrono>

namespace hft {

MarketDataFeed::MarketDataFeed(std::string feed_name)
    : name_(std::move(feed_name)) {}

MarketDataFeed::~MarketDataFeed() {
    stop();
}

void MarketDataFeed::register_handler(TickHandler handler) {
    handler_ = std::move(handler);
}

void MarketDataFeed::start() {
    if (running_.exchange(true)) return;  // already running
    consumer_thread_ = std::thread(&MarketDataFeed::consumer_loop, this);
}

void MarketDataFeed::stop() {
    if (!running_.exchange(false)) return;  // already stopped
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
}

bool MarketDataFeed::publish(const MarketDataTick& tick) noexcept {
    if (ring_.push(tick)) {
        published_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MarketDataFeed::consumer_loop() {
    while (running_.load(std::memory_order_acquire) || !ring_.empty()) {
        auto tick_opt = ring_.pop();
        if (tick_opt) {
            if (handler_) handler_(*tick_opt);
            consumed_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // No data — yield to avoid spinning at 100% CPU when idle
            std::this_thread::yield();
        }
    }
}

uint64_t MarketDataFeed::ticks_published() const noexcept {
    return published_.load(std::memory_order_relaxed);
}

uint64_t MarketDataFeed::ticks_consumed() const noexcept {
    return consumed_.load(std::memory_order_relaxed);
}

uint64_t MarketDataFeed::ticks_dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

} // namespace hft