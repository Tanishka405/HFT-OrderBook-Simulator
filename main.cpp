#include "order_book.hpp"
#include "market_data_feed.hpp"
#include "latency_timer.hpp"
#include "memory_pool.hpp"
#include "spsc_ring_buffer.hpp"

#include <iostream>
#include <iomanip>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <sstream>
#include <cassert>
#include <cstring>

using namespace hft;
using namespace std::chrono_literals;

// ─── Console helpers ──────────────────────────────────────────────────────────
namespace {

void print_header(const std::string& title) {
    const std::string line(60, '=');
    std::cout << "\n" << line << "\n";
    std::cout << "  " << title << "\n";
    std::cout << line << "\n";
}

void print_depth_snapshot(const OrderBook& book, std::size_t levels = 5) {
    auto asks = book.ask_depth(levels);
    auto bids = book.bid_depth(levels);

    std::cout << "\n  ┌─────────────────────────────────────────┐\n";
    std::cout << "  │ ORDER BOOK: " << std::setw(27) << std::left
              << book.symbol() << "│\n";
    std::cout << "  ├──────────┬──────────┬────────────────────┤\n";
    std::cout << "  │   Price  │    Qty   │      Side          │\n";
    std::cout << "  ├──────────┼──────────┼────────────────────┤\n";

    // Print asks in reverse (highest first visually)
    for (int i = static_cast<int>(asks.size()) - 1; i >= 0; --i) {
        std::cout << "  │ " << std::setw(8) << std::right
                  << std::fixed << std::setprecision(2)
                  << (asks[i].price / 100.0)
                  << " │ " << std::setw(8) << asks[i].qty
                  << " │  ASK                 │\n";
    }

    std::cout << "  ├──────────┼──────────┼────────────────────┤\n";

    for (const auto& b : bids) {
        std::cout << "  │ " << std::setw(8) << std::right
                  << std::fixed << std::setprecision(2)
                  << (b.price / 100.0)
                  << " │ " << std::setw(8) << b.qty
                  << " │  BID                 │\n";
    }

    std::cout << "  └──────────┴──────────┴────────────────────┘\n";
    std::cout << "  Spread: $" << std::fixed << std::setprecision(4)
              << (book.spread() / 100.0) << "\n";
}

} // anonymous namespace

// ─── Test 1: Memory Pool ─────────────────────────────────────────────────────
void test_memory_pool() {
    print_header("TEST 1: Memory Pool — Lock-Free Allocator");

    constexpr std::size_t POOL_CAP = 1024;
    MemoryPool<Order, POOL_CAP> pool;

    assert(pool.available() == POOL_CAP);
    assert(pool.in_use()    == 0);

    LatencyTimer alloc_timer("MemPool::allocate", POOL_CAP);
    std::vector<Order*> ptrs;
    ptrs.reserve(POOL_CAP);

    // Allocation burst
    for (std::size_t i = 0; i < POOL_CAP; ++i) {
        auto t0 = alloc_timer.start();
        Order* o = pool.allocate();
        alloc_timer.record(t0);
        assert(o != nullptr);
        o->id       = i + 1;
        o->price    = 10000 + static_cast<Price>(i);
        o->quantity = 100;
        ptrs.push_back(o);
    }

    // Pool should be exhausted
    assert(pool.available() == 0);
    assert(pool.allocate()  == nullptr);

    // Deallocation burst
    LatencyTimer dealloc_timer("MemPool::deallocate", POOL_CAP);
    for (Order* o : ptrs) {
        auto t0 = dealloc_timer.start();
        pool.deallocate(o);
        dealloc_timer.record(t0);
    }
    assert(pool.available() == POOL_CAP);

    std::cout << alloc_timer.report();
    std::cout << dealloc_timer.report();
    std::cout << "  [PASS] MemoryPool: allocate/deallocate cycle verified.\n";
}

// ─── Test 2: SPSC Ring Buffer ─────────────────────────────────────────────────
void test_spsc_ring_buffer() {
    print_header("TEST 2: SPSC Ring Buffer — Lock-Free Queue");

    constexpr std::size_t RING_CAP = 4096;
    constexpr std::size_t MSG_COUNT = 1'000'000;

    SPSCRingBuffer<MarketDataTick, RING_CAP> ring;

    std::atomic<bool>    start_flag{false};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};

    auto t_start = std::chrono::steady_clock::now();

    // Producer thread
    std::thread producer([&] {
        while (!start_flag.load(std::memory_order_acquire)) { /* spin-wait */ }

        MarketDataTick tick{};
        tick.best_bid = 10000;
        tick.best_ask = 10005;

        for (std::size_t i = 0; i < MSG_COUNT; ++i) {
            tick.timestamp = static_cast<Timestamp>(i);
            while (!ring.push(tick)) {
                // Spin on full — back-pressure
                std::this_thread::yield();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Consumer thread
    std::thread consumer([&] {
        while (!start_flag.load(std::memory_order_acquire)) { /* spin-wait */ }

        while (consumed.load(std::memory_order_relaxed) < MSG_COUNT) {
            auto tick = ring.pop();
            if (tick) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start_flag.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    auto t_end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        t_end - t_start).count();

    double throughput = static_cast<double>(MSG_COUNT) /
                        (static_cast<double>(elapsed_us) / 1e6);

    assert(produced.load() == MSG_COUNT);
    assert(consumed.load() == MSG_COUNT);

    std::cout << "  Messages  : " << MSG_COUNT << "\n";
    std::cout << "  Elapsed   : " << elapsed_us << " µs\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "  Throughput: " << throughput << " msg/sec\n";
    std::cout << "  [PASS] SPSC Ring Buffer: " << MSG_COUNT
              << " messages transferred without loss.\n";
}

// ─── Test 3: Order Book Matching Engine ──────────────────────────────────────
void test_order_book_matching() {
    print_header("TEST 3: Order Book — Price-Time Priority Matching");

    std::vector<Trade> trades;
    std::vector<MarketDataTick> ticks;

    OrderBook book("AAPL",
        [&](const Trade& t)        { trades.push_back(t); },
        [&](const MarketDataTick& m) { ticks.push_back(m); }
    );

    // Seed the book with resting limit orders
    // Asks (sell side)
    book.add_order(Side::ASK, OrderType::LIMIT, 15010, 500);  // $150.10
    book.add_order(Side::ASK, OrderType::LIMIT, 15005, 300);  // $150.05
    book.add_order(Side::ASK, OrderType::LIMIT, 15000, 200);  // $150.00  <- best ask

    // Bids (buy side)
    book.add_order(Side::BID, OrderType::LIMIT, 14990, 400);  // $149.90
    book.add_order(Side::BID, OrderType::LIMIT, 14995, 350);  // $149.95
    book.add_order(Side::BID, OrderType::LIMIT, 14998, 250);  // $149.98  <- best bid

    std::cout << "\n  [Initial book seeded]\n";
    print_depth_snapshot(book);

    assert(book.best_bid() == 14998);
    assert(book.best_ask() == 15000);
    assert(book.spread()   == 2);  // $0.02

    // Aggressive market buy — should sweep asks
    auto t0 = trades.size();
    book.add_order(Side::BID, OrderType::MARKET, 0, 600);
    assert(trades.size() > t0);  // at least one trade must have fired

    std::cout << "\n  [After aggressive market buy (qty=600)]\n";
    print_depth_snapshot(book);

    // IOC order that partially fills then cancels residual
    book.add_order(Side::ASK, OrderType::LIMIT, 15005, 1000);  // refill ask side
    std::size_t pre_ioc_trades = trades.size();
    book.add_order(Side::BID, OrderType::IOC, 15005, 2000);    // more than available
    std::size_t post_ioc_trades = trades.size();
    // Should match what's available; residual cancelled
    assert(post_ioc_trades > pre_ioc_trades);

    // FOK that cannot fill entirely — must be rejected
    std::size_t pre_fok_trades = trades.size();
    book.add_order(Side::BID, OrderType::FOK, 15010, 99999);   // enormous qty
    std::size_t post_fok_trades = trades.size();
    assert(pre_fok_trades == post_fok_trades);  // no trades should have fired

    std::cout << "\n  Trades executed : " << trades.size() << "\n";
    std::cout << "  Total volume    : " << book.total_volume() << " shares\n";
    std::cout << "  [PASS] Matching engine: LIMIT, MARKET, IOC, FOK verified.\n";
}

// ─── Test 4: Full Simulation Under Load ──────────────────────────────────────
void test_simulation_under_load() {
    print_header("TEST 4: Load Simulation — Dynamic Order Flow");

    constexpr int    NUM_ORDERS  = 50'000;
    constexpr Price  BASE_PRICE  = 10000;   // $100.00
    constexpr int    PRICE_RANGE = 100;     // ±$0.50 around mid

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int>      price_dist(-PRICE_RANGE, PRICE_RANGE);
    std::uniform_int_distribution<Quantity> qty_dist(1, 500);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      type_dist(0, 3);

    std::atomic<uint64_t> trade_count{0};
    std::atomic<uint64_t> volume{0};

    OrderBook book("SPY",
        [&](const Trade& t) {
            trade_count.fetch_add(1, std::memory_order_relaxed);
            volume.fetch_add(t.quantity, std::memory_order_relaxed);
        },
        nullptr
    );

    LatencyTimer order_latency("add_order", NUM_ORDERS);
    std::vector<OrderId> active_ids;
    active_ids.reserve(NUM_ORDERS / 4);

    auto sim_start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_ORDERS; ++i) {
        Side      side  = static_cast<Side>(side_dist(rng));
        OrderType otype = static_cast<OrderType>(type_dist(rng) % 2);  // LIMIT or MARKET
        Price     px    = BASE_PRICE + price_dist(rng);
        Quantity  qty   = qty_dist(rng);

        auto t0 = order_latency.start();
        OrderId oid = book.add_order(side, otype, px, qty);
        order_latency.record(t0);

        if (oid && otype == OrderType::LIMIT && (i % 7 != 0)) {
            active_ids.push_back(oid);
        }

        // Periodically cancel a random resting order (simulates algo cancel/replace)
        if (!active_ids.empty() && (i % 5 == 0)) {
            std::uniform_int_distribution<std::size_t> idx_dist(0, active_ids.size() - 1);
            std::size_t idx = idx_dist(rng);
            book.cancel_order(active_ids[idx]);
            active_ids.erase(active_ids.begin() + static_cast<ptrdiff_t>(idx));
        }
    }

    auto sim_end = std::chrono::steady_clock::now();
    auto sim_us  = std::chrono::duration_cast<std::chrono::microseconds>(
        sim_end - sim_start).count();

    std::cout << "\n" << order_latency.report();
    std::cout << "  Simulation elapsed  : " << sim_us << " µs\n";
    std::cout << "  Total orders        : " << NUM_ORDERS << "\n";
    std::cout << "  Orders/sec          : " << std::fixed << std::setprecision(0)
              << (NUM_ORDERS / (sim_us / 1e6)) << "\n";
    std::cout << "  Trades executed     : " << trade_count.load() << "\n";
    std::cout << "  Total volume        : " << volume.load() << " units\n";
    std::cout << "  Remaining bid levels: " << book.bid_levels() << "\n";
    std::cout << "  Remaining ask levels: " << book.ask_levels() << "\n";
    std::cout << "  Remaining orders    : " << book.order_count() << "\n";
    print_depth_snapshot(book);
    std::cout << "  [PASS] Load simulation complete.\n";
}

// ─── Test 5: Market Data Feed (SPSC producer → consumer) ─────────────────────
void test_market_data_feed() {
    print_header("TEST 5: Market Data Feed — Async SPSC Pipeline");

    constexpr int TICK_COUNT = 200'000;
    std::atomic<uint64_t> received{0};

    MarketDataFeed feed("FAST-FEED");
    feed.register_handler([&](const MarketDataTick& tick) {
        (void)tick;
        received.fetch_add(1, std::memory_order_relaxed);
    });
    feed.start();

    auto t_start = std::chrono::steady_clock::now();

    MarketDataTick tick{};
    tick.best_bid = 15000;
    tick.best_ask = 15002;
    for (int i = 0; i < TICK_COUNT; ++i) {
        tick.timestamp = static_cast<Timestamp>(i);
        while (!feed.publish(tick)) {
            std::this_thread::yield();
        }
    }

    // Wait for consumer to drain
    while (received.load(std::memory_order_acquire) < TICK_COUNT) {
        std::this_thread::sleep_for(100us);
    }

    feed.stop();

    auto t_end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        t_end - t_start).count();

    assert(feed.ticks_published() >= static_cast<uint64_t>(TICK_COUNT));
    assert(received.load() >= static_cast<uint64_t>(TICK_COUNT));

    std::cout << "  Ticks published : " << feed.ticks_published() << "\n";
    std::cout << "  Ticks consumed  : " << feed.ticks_consumed() << "\n";
    std::cout << "  Ticks dropped   : " << feed.ticks_dropped() << "\n";
    std::cout << "  Elapsed         : " << elapsed_us << " µs\n";
    std::cout << "  Throughput      : " << std::fixed << std::setprecision(0)
              << (TICK_COUNT / (elapsed_us / 1e6)) << " ticks/sec\n";
    std::cout << "  [PASS] Market data feed pipeline verified.\n";
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::cout << R"(
  ╔═══════════════════════════════════════════════════════════╗
  ║   HFT Market Data & Order Book Simulator  v1.0            ║
  ║   Ultra-Low Latency Trading Infrastructure                ║
  ╚═══════════════════════════════════════════════════════════╝
)";

    try {
        test_memory_pool();
        test_spsc_ring_buffer();
        test_order_book_matching();
        test_simulation_under_load();
        test_market_data_feed();
    } catch (const std::exception& ex) {
        std::cerr << "\n[FATAL] Unhandled exception: " << ex.what() << "\n";
        return 1;
    }

    print_header("ALL TESTS PASSED");
    std::cout << "  System verified: memory pool, ring buffer, order book,\n";
    std::cout << "  matching engine, and async market data feed.\n\n";
    return 0;
}