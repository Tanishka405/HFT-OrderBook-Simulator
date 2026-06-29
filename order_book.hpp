#pragma once

#include "order_types.hpp"
#include "memory_pool.hpp"

#include <map>
#include <unordered_map>
#include <functional>
#include <vector>
#include <limits>
#include <cstddef>
#include <memory>

namespace hft {

// ── Price Level ───────────────────────────────────────────────────────────────
// An intrusive linked list of Orders at a single price point.
// All pointers here are pool-owned — no heap allocations.
struct alignas(64) PriceLevel {
    Price     price     = 0;
    Quantity  total_qty = 0;
    Order*    head      = nullptr;  // oldest (FIFO front)
    Order*    tail      = nullptr;  // newest  (FIFO back)
    std::size_t order_count = 0;

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else       head      = o;
        tail = o;
        total_qty  += o->quantity;
        ++order_count;
    }

    // Remove an arbitrary order in O(1) via its intrusive links.
    void remove(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head          = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail          = o->prev;
        total_qty -= o->remaining();
        --order_count;
        o->prev = o->next = nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

// ── Callbacks ─────────────────────────────────────────────────────────────────
using TradeCallback     = std::function<void(const Trade&)>;
using MarketDataCallback = std::function<void(const MarketDataTick&)>;

// ── Order Book ────────────────────────────────────────────────────────────────
/**
 * @brief Central limit order book (CLOB) with price-time priority matching.
 *
 * Architecture:
 *  - Bids: std::map<Price, PriceLevel, std::greater<>> — highest bid first
 *  - Asks: std::map<Price, PriceLevel>                 — lowest  ask first
 *  - Order lookup: std::unordered_map<OrderId, Order*>  — O(1) cancel/amend
 *  - All Order objects are allocated from a MemoryPool — zero heap traffic on
 *    the critical path.
 *
 * Matching semantics:
 *  - LIMIT  : rests if no cross; partial fills allowed
 *  - MARKET : sweeps until filled or book exhausted
 *  - IOC    : matches immediately, cancels residual
 *  - FOK    : checks full fillability before touching the book; rejects if not
 *
 * Thread safety: NOT thread-safe by design.  Callers must serialize access or
 * partition books by symbol (one book per thread).
 */
class OrderBook {
public:
    static constexpr std::size_t MAX_ORDERS = 65536;

    explicit OrderBook(std::string symbol,
                       TradeCallback      on_trade     = nullptr,
                       MarketDataCallback on_market_data = nullptr);

    ~OrderBook() = default;

    // Non-copyable; books own pool resources
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = default;
    OrderBook& operator=(OrderBook&&)      = default;

    // ── Primary API ──────────────────────────────────────────────────────────

    /**
     * @brief Submit a new order. Returns its assigned OrderId (never 0).
     *        Returns 0 if the pool is exhausted.
     */
    [[nodiscard]] OrderId add_order(Side side,
                                    OrderType type,
                                    Price     price,
                                    Quantity  quantity);

    /**
     * @brief Cancel an existing order. Returns false if OrderId not found.
     */
    bool cancel_order(OrderId id);

    /**
     * @brief Amend (replace) the quantity of a resting order.
     *        Amending to a larger qty loses time priority; reducing keeps it.
     */
    bool amend_order(OrderId id, Quantity new_qty);

    // ── Market Data Queries ───────────────────────────────────────────────────

    [[nodiscard]] Price    best_bid()     const noexcept;
    [[nodiscard]] Price    best_ask()     const noexcept;
    [[nodiscard]] Quantity best_bid_qty() const noexcept;
    [[nodiscard]] Quantity best_ask_qty() const noexcept;
    [[nodiscard]] Price    spread()       const noexcept;

    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return order_map_.size(); }

    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

    // ── Statistics ───────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t total_trades()    const noexcept { return stats_.total_trades; }
    [[nodiscard]] uint64_t total_volume()    const noexcept { return stats_.total_volume; }
    [[nodiscard]] uint64_t orders_added()    const noexcept { return stats_.orders_added; }
    [[nodiscard]] uint64_t orders_cancelled()const noexcept { return stats_.orders_cancelled; }

    // ── Depth Snapshot ───────────────────────────────────────────────────────
    struct DepthEntry { Price price; Quantity qty; std::size_t order_cnt; };

    [[nodiscard]] std::vector<DepthEntry> bid_depth(std::size_t levels = 10) const;
    [[nodiscard]] std::vector<DepthEntry> ask_depth(std::size_t levels = 10) const;

private:
    // ── Matching Engine Core ─────────────────────────────────────────────────
    void match_order(Order* incoming);
    void match_against_asks(Order* bid);
    void match_against_bids(Order* ask);
    Quantity  check_fok_fillable(const Order* incoming) const noexcept;

    void execute_trade(Order* maker, Order* taker, Price px, Quantity qty);
    void rest_order(Order* o);
    void remove_from_book(Order* o);
    void publish_market_data();

    // ── Internal helpers ──────────────────────────────────────────────────────
    OrderId next_id() noexcept { return ++id_counter_; }

    // ── Book sides ────────────────────────────────────────────────────────────
    // std::greater so bids_.begin() is always the best (highest) bid.
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel>                       asks_;

    // O(1) order lookup by ID for cancels/amends
    std::unordered_map<OrderId, Order*> order_map_;

    // Pool — heap-allocated to avoid blowing the stack (65536 * 128B = 8MB)
    std::unique_ptr<MemoryPool<Order, MAX_ORDERS>> pool_;

    std::string        symbol_;
    OrderId            id_counter_ = 0;
    Price              last_trade_px_ = 0;
    Quantity           last_trade_qty_ = 0;

    TradeCallback      on_trade_;
    MarketDataCallback on_market_data_;

    struct Stats {
        uint64_t total_trades    = 0;
        uint64_t total_volume    = 0;
        uint64_t orders_added    = 0;
        uint64_t orders_cancelled= 0;
    } stats_;
};

} // namespace hft