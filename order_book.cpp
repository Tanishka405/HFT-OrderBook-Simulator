#include "order_book.hpp"
#include <stdexcept>
#include <limits>

namespace hft {

// ── Constructor ───────────────────────────────────────────────────────────────
OrderBook::OrderBook(std::string symbol,
                     TradeCallback      on_trade,
                     MarketDataCallback on_market_data)
    : symbol_(std::move(symbol))
    , on_trade_(std::move(on_trade))
    , on_market_data_(std::move(on_market_data))
    , pool_(std::make_unique<MemoryPool<Order, MAX_ORDERS>>())
{}

// ── Add Order ─────────────────────────────────────────────────────────────────
OrderId OrderBook::add_order(Side side, OrderType type, Price price, Quantity quantity) {
    if (quantity == 0) return 0;
    if (type == OrderType::LIMIT && price <= 0) return 0;

    // Allocate from pool — zero heap traffic
    Order* o = pool_->allocate();
    if (!o) [[unlikely]] return 0;  // pool exhausted

    o->id         = next_id();
    o->price      = price;
    o->quantity   = quantity;
    o->filled_qty = 0;
    o->side       = side;
    o->type       = type;
    o->status     = OrderStatus::NEW;
    o->timestamp  = now_ns();
    o->prev = o->next = nullptr;

    order_map_.emplace(o->id, o);
    ++stats_.orders_added;

    match_order(o);

    return o->id;
}

// ── Cancel Order ──────────────────────────────────────────────────────────────
bool OrderBook::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;

    Order* o = it->second;
    if (!o->is_active()) return false;

    remove_from_book(o);
    o->status = OrderStatus::CANCELLED;
    order_map_.erase(it);
    ++stats_.orders_cancelled;

    pool_->deallocate(o);
    publish_market_data();
    return true;
}

// ── Amend Order ───────────────────────────────────────────────────────────────
bool OrderBook::amend_order(OrderId id, Quantity new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;

    Order* o = it->second;
    if (!o->is_active()) return false;
    if (new_qty <= o->filled_qty) return false;

    const bool increasing = (new_qty > o->quantity);

    if (increasing) {
        // Lose time priority: remove and re-insert at back of level
        remove_from_book(o);
        o->quantity = new_qty;
        rest_order(o);
    } else {
        // Reduce in place — keep time priority
        if (o->side == Side::BID) {
            auto level_it = bids_.find(o->price);
            if (level_it != bids_.end()) {
                level_it->second.total_qty -= (o->quantity - new_qty);
            }
        } else {
            auto level_it = asks_.find(o->price);
            if (level_it != asks_.end()) {
                level_it->second.total_qty -= (o->quantity - new_qty);
            }
        }
        o->quantity = new_qty;
    }

    publish_market_data();
    return true;
}

// ── Matching Engine ───────────────────────────────────────────────────────────
void OrderBook::match_order(Order* incoming) {
    if (incoming->type == OrderType::FOK) {
        Quantity fillable = check_fok_fillable(incoming);
        if (fillable < incoming->quantity) {
            // Cannot fill entirely — reject without touching the book
            incoming->status = OrderStatus::REJECTED;
            order_map_.erase(incoming->id);
            pool_->deallocate(incoming);
            return;
        }
    }

    if (incoming->side == Side::BID) {
        match_against_asks(incoming);
    } else {
        match_against_bids(incoming);
    }

    // If not fully filled: rest (LIMIT) or cancel residual (IOC/FOK/MARKET)
    if (incoming->is_active()) {
        if (incoming->type == OrderType::LIMIT) {
            rest_order(incoming);
        } else {
            // IOC / FOK / MARKET — cancel residual
            incoming->status = OrderStatus::CANCELLED;
            order_map_.erase(incoming->id);
            pool_->deallocate(incoming);
        }
    }

    publish_market_data();
}

void OrderBook::match_against_asks(Order* bid) {
    while (bid->is_active() && !asks_.empty()) {
        auto ask_it = asks_.begin();  // best (lowest) ask
        PriceLevel& level = ask_it->second;

        // Price check: bid must cross the ask
        if (bid->type == OrderType::LIMIT && bid->price < level.price) break;

        Price match_px = level.price;  // passive (maker) price
        Order* ask = level.head;

        while (ask && bid->is_active()) {
            Quantity fill_qty = std::min(bid->remaining(), ask->remaining());

            execute_trade(ask, bid, match_px, fill_qty);

            Order* next_ask = ask->next;

            if (ask->remaining() == 0) {
                level.remove(ask);
                ask->status = OrderStatus::FILLED;
                order_map_.erase(ask->id);
                pool_->deallocate(ask);
            }

            ask = next_ask;
        }

        if (level.empty()) {
            asks_.erase(ask_it);
        }
    }
}

void OrderBook::match_against_bids(Order* ask) {
    while (ask->is_active() && !bids_.empty()) {
        auto bid_it = bids_.begin();  // best (highest) bid
        PriceLevel& level = bid_it->second;

        if (ask->type == OrderType::LIMIT && ask->price > level.price) break;

        Price match_px = level.price;
        Order* bid = level.head;

        while (bid && ask->is_active()) {
            Quantity fill_qty = std::min(ask->remaining(), bid->remaining());

            execute_trade(bid, ask, match_px, fill_qty);

            Order* next_bid = bid->next;

            if (bid->remaining() == 0) {
                level.remove(bid);
                bid->status = OrderStatus::FILLED;
                order_map_.erase(bid->id);
                pool_->deallocate(bid);
            }

            bid = next_bid;
        }

        if (level.empty()) {
            bids_.erase(bid_it);
        }
    }
}

Quantity OrderBook::check_fok_fillable(const Order* incoming) const noexcept {
    Quantity available = 0;
    if (incoming->side == Side::BID) {
        for (const auto& [px, level] : asks_) {
            if (incoming->price < px) break;
            available += level.total_qty;
            if (available >= incoming->quantity) return available;
        }
    } else {
        for (const auto& [px, level] : bids_) {
            if (incoming->price > px) break;
            available += level.total_qty;
            if (available >= incoming->quantity) return available;
        }
    }
    return available;
}

void OrderBook::execute_trade(Order* maker, Order* taker, Price px, Quantity qty) {
    maker->filled_qty += qty;
    taker->filled_qty += qty;

    if (maker->remaining() == 0) maker->status = OrderStatus::FILLED;
    else                          maker->status = OrderStatus::PARTIAL;

    if (taker->remaining() == 0) taker->status = OrderStatus::FILLED;
    else                          taker->status = OrderStatus::PARTIAL;

    // Update level aggregate on the maker side
    if (maker->side == Side::BID) {
        auto level_it = bids_.find(maker->price);
        if (level_it != bids_.end()) {
            level_it->second.total_qty -= qty;
        }
    } else {
        auto level_it = asks_.find(maker->price);
        if (level_it != asks_.end()) {
            level_it->second.total_qty -= qty;
        }
    }

    last_trade_px_  = px;
    last_trade_qty_ = qty;

    ++stats_.total_trades;
    stats_.total_volume += qty;

    if (on_trade_) {
        Trade t;
        t.maker_order_id = maker->id;
        t.taker_order_id = taker->id;
        t.price          = px;
        t.quantity       = qty;
        t.timestamp      = now_ns();
        on_trade_(t);
    }
}

void OrderBook::rest_order(Order* o) {
    // push_back adds o->quantity to total_qty, but the order may have
    // been partially filled as a taker before resting, so correct for that.
    if (o->side == Side::BID) {
        auto& level = bids_[o->price];
        level.price = o->price;
        level.push_back(o);
        // push_back used o->quantity; adjust down for any taker fills
        level.total_qty -= o->filled_qty;
    } else {
        auto& level = asks_[o->price];
        level.price = o->price;
        level.push_back(o);
        level.total_qty -= o->filled_qty;
    }
}

void OrderBook::remove_from_book(Order* o) {
    if (o->side == Side::BID) {
        auto level_it = bids_.find(o->price);
        if (level_it == bids_.end()) return;
        level_it->second.remove(o);
        if (level_it->second.empty()) bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(o->price);
        if (level_it == asks_.end()) return;
        level_it->second.remove(o);
        if (level_it->second.empty()) asks_.erase(level_it);
    }
}

void OrderBook::publish_market_data() {
    if (!on_market_data_) return;

    MarketDataTick tick;
    tick.timestamp      = now_ns();
    tick.last_trade_px  = last_trade_px_;
    tick.last_trade_qty = last_trade_qty_;

    if (!bids_.empty()) {
        const auto& [px, lvl] = *bids_.begin();
        tick.best_bid     = px;
        tick.best_bid_qty = lvl.total_qty;
    }
    if (!asks_.empty()) {
        const auto& [px, lvl] = *asks_.begin();
        tick.best_ask     = px;
        tick.best_ask_qty = lvl.total_qty;
    }

    on_market_data_(tick);
}

// ── Market Data Queries ───────────────────────────────────────────────────────
Price OrderBook::best_bid() const noexcept {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const noexcept {
    return asks_.empty() ? std::numeric_limits<Price>::max() : asks_.begin()->first;
}

Quantity OrderBook::best_bid_qty() const noexcept {
    return bids_.empty() ? 0 : bids_.begin()->second.total_qty;
}

Quantity OrderBook::best_ask_qty() const noexcept {
    return asks_.empty() ? 0 : asks_.begin()->second.total_qty;
}

Price OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return 0;
    return best_ask() - best_bid();
}

std::vector<OrderBook::DepthEntry> OrderBook::bid_depth(std::size_t levels) const {
    std::vector<DepthEntry> result;
    result.reserve(levels);
    for (const auto& [px, lvl] : bids_) {
        if (result.size() >= levels) break;
        result.push_back({px, lvl.total_qty, lvl.order_count});
    }
    return result;
}

std::vector<OrderBook::DepthEntry> OrderBook::ask_depth(std::size_t levels) const {
    std::vector<DepthEntry> result;
    result.reserve(levels);
    for (const auto& [px, lvl] : asks_) {
        if (result.size() >= levels) break;
        result.push_back({px, lvl.total_qty, lvl.order_count});
    }
    return result;
}

} // namespace hft