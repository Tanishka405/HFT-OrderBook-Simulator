#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace hft {

// ── Typedefs ─────────────────────────────────────────────────────────────────
using OrderId    = uint64_t;
using Price      = int64_t;   // fixed-point: price * 100  (e.g. 10050 = $100.50)
using Quantity   = uint32_t;
using Timestamp  = int64_t;   // nanoseconds since epoch

// ── Order Side ───────────────────────────────────────────────────────────────
enum class Side : uint8_t {
    BID = 0,  // buy
    ASK = 1,  // sell
};

// ── Order Types ──────────────────────────────────────────────────────────────
enum class OrderType : uint8_t {
    LIMIT  = 0,
    MARKET = 1,
    IOC    = 2,  // Immediate-Or-Cancel
    FOK    = 3,  // Fill-Or-Kill
};

// ── Order Status ─────────────────────────────────────────────────────────────
enum class OrderStatus : uint8_t {
    NEW         = 0,
    PARTIAL     = 1,
    FILLED      = 2,
    CANCELLED   = 3,
    REJECTED    = 4,
};

// ── Core Order ───────────────────────────────────────────────────────────────
// alignas(64) keeps each Order on a single cache line preventing false sharing
// when multiple threads traverse different price levels.
struct alignas(64) Order {
    OrderId    id          = 0;
    Price      price       = 0;
    Quantity   quantity    = 0;
    Quantity   filled_qty  = 0;
    Side       side        = Side::BID;
    OrderType  type        = OrderType::LIMIT;
    OrderStatus status     = OrderStatus::NEW;
    Timestamp  timestamp   = 0;

    // Intrusive doubly-linked list pointers for O(1) removal from price level
    Order* prev = nullptr;
    Order* next = nullptr;

    [[nodiscard]] Quantity remaining() const noexcept {
        return quantity - filled_qty;
    }
    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::NEW || status == OrderStatus::PARTIAL;
    }
};

// ── Trade / Fill ─────────────────────────────────────────────────────────────
struct alignas(64) Trade {
    OrderId   maker_order_id = 0;  // resting order
    OrderId   taker_order_id = 0;  // aggressor
    Price     price          = 0;
    Quantity  quantity       = 0;
    Timestamp timestamp      = 0;
};

// ── Market Data Tick ─────────────────────────────────────────────────────────
// Represents a top-of-book snapshot published after each match cycle.
struct alignas(64) MarketDataTick {
    Price    best_bid       = 0;
    Price    best_ask       = 0;
    Quantity best_bid_qty   = 0;
    Quantity best_ask_qty   = 0;
    Price    last_trade_px  = 0;
    Quantity last_trade_qty = 0;
    Timestamp timestamp     = 0;
};

// ── Utility ──────────────────────────────────────────────────────────────────
[[nodiscard]] inline Timestamp now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
}

} // namespace hft