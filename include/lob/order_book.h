#pragma once
#include <algorithm>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/publisher.h"
#include "lob/types.h"

namespace lob {

struct Order {
    OrderId   id;
    ClientId  client;
    Side      side;
    Price     price;
    Quantity  qty;
    Timestamp ts;
    Order* prev = nullptr;
    Order* next = nullptr;  // vs jxm35: doubles as OrderPool freelist link — no shared_ptr, no atomic refcount on the hot path
};

struct Trade {
    OrderId   taker;
    OrderId   maker;
    Price     price;
    Quantity  qty;
    Timestamp ts;
    bool      buyer_aggressed;
};

class Limit {
public:
    explicit Limit(Price p) noexcept : price_(p) {}

    [[nodiscard]] Price    price() const noexcept { return price_; }
    [[nodiscard]] Quantity total() const noexcept { return total_qty_; }
    [[nodiscard]] uint32_t count() const noexcept { return order_count_; }
    [[nodiscard]] bool     empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] Order*   head()  const noexcept { return head_; }

    void push_back(Order* o) noexcept {
        o->prev = tail_;
        o->next = nullptr;
        if (tail_) tail_->next = o;
        else       head_ = o;
        tail_ = o;
        total_qty_ += o->qty;
        ++order_count_;
    }

    void unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head_ = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail_ = o->prev;
        o->prev = o->next = nullptr;
        --order_count_;
    }

    void decrease(Quantity q) noexcept { total_qty_ -= q; }

private:
    Price    price_;
    Order*   head_        = nullptr;
    Order*   tail_        = nullptr;
    Quantity total_qty_   = 0;
    uint32_t order_count_ = 0;
};

class OrderPool {
public:
    explicit OrderPool(size_t capacity) : slab_(capacity) {
        if (capacity == 0) return;
        for (size_t i = 0; i + 1 < slab_.size(); ++i)
            slab_[i].next = &slab_[i + 1];
        slab_.back().next = nullptr;
        free_ = &slab_[0];
    }

    Order* acquire() {
#ifdef LOB_NO_POOL
        ++in_use_;
        return new Order{};
#else
        if (!free_) return nullptr;
        Order* o = free_;
        free_ = o->next;
        *o = Order{};
        ++in_use_;
        return o;
#endif
    }

    void release(Order* o) {
#ifdef LOB_NO_POOL
        delete o;
        --in_use_;
#else
        o->next = free_;
        free_ = o;
        --in_use_;
#endif
    }

    [[nodiscard]] size_t in_use()   const noexcept { return in_use_; }
    [[nodiscard]] size_t capacity() const noexcept { return slab_.size(); }

private:
    std::vector<Order> slab_;
    Order* free_   = nullptr;
    size_t in_use_ = 0;
};

template <Publisher Pub = NullPublisher>
class OrderBook {
public:
    explicit OrderBook(size_t pool_capacity = 1 << 20)
        requires std::is_default_constructible_v<Pub>
        : pool_(pool_capacity) {}

    OrderBook(Pub publisher, size_t pool_capacity = 1 << 20)
        : publisher_(std::move(publisher)), pool_(pool_capacity) {}

    std::vector<Trade> submit_limit (OrderId id, ClientId c, Side s, Price p, Quantity q, Timestamp ts);
    std::vector<Trade> submit_market(OrderId id, ClientId c, Side s,          Quantity q, Timestamp ts);
    std::expected<void, std::string> cancel(OrderId id);  // vs jxm35: std::expected vs bool — typed errors, no exception cost

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Quantity volume_at(Side s, Price p) const noexcept;
    [[nodiscard]] uint32_t orders_at(Side s, Price p) const noexcept;
    [[nodiscard]] size_t   open_orders() const noexcept { return id_index_.size(); }
    [[nodiscard]] bool     contains(OrderId id) const noexcept { return id_index_.contains(id); }

    Pub&       publisher()       noexcept { return publisher_; }
    const Pub& publisher() const noexcept { return publisher_; }

private:
    Pub       publisher_{};
    OrderPool pool_;

    std::map<Price, Limit, std::greater<Price>> bids_;
    std::map<Price, Limit, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Order*>         id_index_;

    OrderId next_trade_id_ = 1;

    template <class BookSide>
    void match(BookSide& opposite, Order* taker, std::vector<Trade>& out);

    template <class BookSide>
    void rest_on(BookSide& book, Order* o);
};

template <Publisher Pub>
std::optional<Price> OrderBook<Pub>::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

template <Publisher Pub>
std::optional<Price> OrderBook<Pub>::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

template <Publisher Pub>
Quantity OrderBook<Pub>::volume_at(Side s, Price p) const noexcept {
    if (s == Side::Buy) {
        auto it = bids_.find(p);
        return it == bids_.end() ? 0 : it->second.total();
    }
    auto it = asks_.find(p);
    return it == asks_.end() ? 0 : it->second.total();
}

template <Publisher Pub>
uint32_t OrderBook<Pub>::orders_at(Side s, Price p) const noexcept {
    if (s == Side::Buy) {
        auto it = bids_.find(p);
        return it == bids_.end() ? 0 : it->second.count();
    }
    auto it = asks_.find(p);
    return it == asks_.end() ? 0 : it->second.count();
}

template <Publisher Pub>
template <class BookSide>
void OrderBook<Pub>::match(BookSide& opposite, Order* taker, std::vector<Trade>& out) {
    auto crosses = [&](Price book_px) noexcept {
        return taker->side == Side::Buy ? taker->price >= book_px
                                        : taker->price <= book_px;
    };

    // vs jxm35: re-fetch begin() per outer iter — no erasedLimit flag, iterator invalidation impossible
    while (taker->qty > 0 && !opposite.empty()) {
        auto it = opposite.begin();
        if (!crosses(it->first)) return;

        Limit&         lvl        = it->second;
        const Quantity old_total  = lvl.total();
        const Side     maker_side = (taker->side == Side::Buy) ? Side::Sell : Side::Buy;

        while (taker->qty > 0 && lvl.head()) {
            Order* maker = lvl.head();

            if (maker->client != 0 && maker->client == taker->client) {
                if (lvl.total() != old_total) {
                    publisher_.on_level_change(it->first, lvl.total(), old_total, maker_side);
                }
                return;
            }

            const Quantity fill = std::min(taker->qty, maker->qty);

            out.push_back(Trade{
                taker->id, maker->id, it->first, fill, taker->ts,
                taker->side == Side::Buy
            });
            publisher_.on_trade(next_trade_id_++, it->first, fill, taker->side);

            taker->qty  -= fill;
            maker->qty  -= fill;
            lvl.decrease(fill);

            if (maker->qty == 0) {
                lvl.unlink(maker);
                id_index_.erase(maker->id);
                pool_.release(maker);
            }
        }

        // vs jxm35: one level event per walked level, not per fill — fewer publisher calls in sweeps
        if (lvl.total() != old_total) {
            publisher_.on_level_change(it->first, lvl.total(), old_total, maker_side);
        }

        if (lvl.empty()) opposite.erase(it);
    }
}

template <Publisher Pub>
template <class BookSide>
void OrderBook<Pub>::rest_on(BookSide& book, Order* o) {
    auto [it, inserted] = book.try_emplace(o->price, o->price);
    const Quantity old_total = it->second.total();
    it->second.push_back(o);
    id_index_.emplace(o->id, o);
    publisher_.on_level_change(o->price, it->second.total(), old_total, o->side);
}

template <Publisher Pub>
std::vector<Trade> OrderBook<Pub>::submit_limit(
    OrderId id, ClientId c, Side s, Price p, Quantity q, Timestamp ts)
{
    std::vector<Trade> trades;
    if (q == 0) return trades;
    if (id_index_.contains(id)) return trades;  // vs jxm35: caller owns id-space; reject duplicates vs static counter

    Order* o = pool_.acquire();
    if (!o) throw std::runtime_error("order pool exhausted");
    *o = Order{id, c, s, p, q, ts, nullptr, nullptr};

    if (s == Side::Buy) {
        match(asks_, o, trades);
        if (o->qty > 0) rest_on(bids_, o);
        else            pool_.release(o);
    } else {
        match(bids_, o, trades);
        if (o->qty > 0) rest_on(asks_, o);
        else            pool_.release(o);
    }
    return trades;
}

template <Publisher Pub>
std::vector<Trade> OrderBook<Pub>::submit_market(
    OrderId id, ClientId c, Side s, Quantity q, Timestamp ts)
{
    std::vector<Trade> trades;
    if (q == 0) return trades;

    Order* o = pool_.acquire();
    if (!o) throw std::runtime_error("order pool exhausted");
    const Price px = s == Side::Buy ? std::numeric_limits<Price>::max()
                                    : std::numeric_limits<Price>::min();
    *o = Order{id, c, s, px, q, ts, nullptr, nullptr};

    if (s == Side::Buy) match(asks_, o, trades);
    else                match(bids_, o, trades);

    pool_.release(o);
    return trades;
}

template <Publisher Pub>
std::expected<void, std::string> OrderBook<Pub>::cancel(OrderId id) {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return std::unexpected("order id not found");

    Order* o = it->second;

    auto cancel_from = [&](auto& book) {
        auto bit = book.find(o->price);
        if (bit == book.end()) return;
        Limit& lvl = bit->second;
        const Quantity old_total = lvl.total();
        lvl.decrease(o->qty);
        lvl.unlink(o);
        publisher_.on_level_change(o->price, lvl.total(), old_total, o->side);
        if (lvl.empty()) book.erase(bit);
    };

    if (o->side == Side::Buy) cancel_from(bids_);
    else                      cancel_from(asks_);

    id_index_.erase(it);
    pool_.release(o);
    return {};
}

}
