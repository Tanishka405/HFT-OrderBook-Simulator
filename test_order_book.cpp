#include "order_book.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace hft;

// ── Helpers ───────────────────────────────────────────────────────────────────
struct TradeLog {
    std::vector<Trade> trades;
    void operator()(const Trade& t) { trades.push_back(t); }
    void clear() { trades.clear(); }
};

// ── Tests ─────────────────────────────────────────────────────────────────────
void test_no_cross_resting() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::BID, OrderType::LIMIT, 9990, 100);
    book.add_order(Side::ASK, OrderType::LIMIT, 10010, 100);

    assert(log.trades.empty());         // no cross — no trades
    assert(book.best_bid() == 9990);
    assert(book.best_ask() == 10010);
    assert(book.spread()   == 20);
    std::cout << "  [PASS] no_cross_resting\n";
}

void test_exact_cross_full_fill() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 200);
    book.add_order(Side::BID, OrderType::LIMIT, 10000, 200);

    assert(log.trades.size() == 1);
    assert(log.trades[0].price    == 10000);
    assert(log.trades[0].quantity == 200);
    assert(book.bid_levels() == 0);
    assert(book.ask_levels() == 0);
    std::cout << "  [PASS] exact_cross_full_fill\n";
}

void test_partial_fill() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 100);
    book.add_order(Side::BID, OrderType::LIMIT, 10000, 300);  // larger bid

    assert(log.trades.size() == 1);
    assert(log.trades[0].quantity == 100);   // only 100 traded
    assert(book.ask_levels() == 0);          // ask fully consumed
    assert(book.best_bid_qty() == 200);      // 200 remains on bid
    std::cout << "  [PASS] partial_fill\n";
}

void test_price_time_priority() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    // Two bids at same price — order 1 has time priority
    OrderId id1 = book.add_order(Side::BID, OrderType::LIMIT, 10000, 50);
    OrderId id2 = book.add_order(Side::BID, OrderType::LIMIT, 10000, 50);
    (void)id2;

    // Sell hits both — should fill id1 first (time priority)
    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 100);

    assert(log.trades.size() == 2);
    assert(log.trades[0].maker_order_id == id1);  // id1 filled first
    std::cout << "  [PASS] price_time_priority\n";
}

void test_market_order_sweep() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 100);
    book.add_order(Side::ASK, OrderType::LIMIT, 10010, 100);
    book.add_order(Side::ASK, OrderType::LIMIT, 10020, 100);

    // Market buy sweeps all three levels
    book.add_order(Side::BID, OrderType::MARKET, 0, 300);

    assert(log.trades.size() == 3);
    assert(book.ask_levels() == 0);
    std::cout << "  [PASS] market_order_sweep\n";
}

void test_ioc_partial_cancel() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 100);

    // IOC wants 300 but only 100 available — fills 100, cancels 200
    book.add_order(Side::BID, OrderType::IOC, 10000, 300);

    assert(log.trades.size() == 1);
    assert(log.trades[0].quantity == 100);
    assert(book.bid_levels() == 0);  // residual 200 must be cancelled, not resting
    std::cout << "  [PASS] ioc_partial_cancel\n";
}

void test_fok_reject_insufficient_qty() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 100);

    // FOK needs 500 — should be rejected outright (no trades, ask intact)
    book.add_order(Side::BID, OrderType::FOK, 10000, 500);

    assert(log.trades.empty());
    assert(book.ask_levels() == 1);  // ask untouched
    std::cout << "  [PASS] fok_reject_insufficient_qty\n";
}

void test_fok_full_fill() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    book.add_order(Side::ASK, OrderType::LIMIT, 10000, 200);
    book.add_order(Side::ASK, OrderType::LIMIT, 10005, 300);

    // FOK needs 500 — exactly available
    book.add_order(Side::BID, OrderType::FOK, 10005, 500);

    assert(log.trades.size() == 2);
    assert(book.ask_levels() == 0);
    std::cout << "  [PASS] fok_full_fill\n";
}

void test_cancel_order() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    OrderId id = book.add_order(Side::BID, OrderType::LIMIT, 10000, 100);
    assert(book.bid_levels() == 1);

    bool ok = book.cancel_order(id);
    assert(ok);
    assert(book.bid_levels() == 0);

    // Double cancel should fail
    assert(!book.cancel_order(id));
    std::cout << "  [PASS] cancel_order\n";
}

void test_amend_order() {
    TradeLog log;
    OrderBook book("TEST", [&](const Trade& t){ log(t); });

    OrderId id = book.add_order(Side::BID, OrderType::LIMIT, 10000, 100);
    assert(book.best_bid_qty() == 100);

    // Reduce qty — keep time priority
    bool ok = book.amend_order(id, 50);
    assert(ok);
    assert(book.best_bid_qty() == 50);
    std::cout << "  [PASS] amend_order\n";
}

int main() {
    std::cout << "=== OrderBook Unit Tests ===\n";
    test_no_cross_resting();
    test_exact_cross_full_fill();
    test_partial_fill();
    test_price_time_priority();
    test_market_order_sweep();
    test_ioc_partial_cancel();
    test_fok_reject_insufficient_qty();
    test_fok_full_fill();
    test_cancel_order();
    test_amend_order();
    std::cout << "All OrderBook tests passed.\n";
    return 0;
}