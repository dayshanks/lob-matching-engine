#pragma once
#include <cstdint>
#include <type_traits>
#include <vector>

#include "lob/types.h"

namespace lob {

template <class Derived>
class PublisherBase {
public:
    void on_level_change(Price price, Quantity new_total, Quantity old_total, Side side) {
        static_cast<Derived*>(this)->on_level_change(price, new_total, old_total, side);
    }
    void on_trade(OrderId trade_id, Price price, Quantity qty, Side aggressor) {
        static_cast<Derived*>(this)->on_trade(trade_id, price, qty, aggressor);
    }
    void on_book_clear() {
        static_cast<Derived*>(this)->on_book_clear();
    }
};

class NullPublisher : public PublisherBase<NullPublisher> {
public:
    static void on_level_change(Price, Quantity, Quantity, Side) noexcept {}
    static void on_trade(OrderId, Price, Quantity, Side) noexcept {}
    static void on_book_clear() noexcept {}
};

class RecordingPublisher : public PublisherBase<RecordingPublisher> {
public:
    struct LevelChange { Price price; Quantity new_total; Quantity old_total; Side side; };
    struct TradeEvent  { OrderId id; Price price; Quantity qty; Side aggressor; };

    std::vector<LevelChange> level_changes;
    std::vector<TradeEvent>  trades;
    int clears = 0;

    void on_level_change(Price p, Quantity nq, Quantity oq, Side s) {
        level_changes.push_back({p, nq, oq, s});
    }
    void on_trade(OrderId id, Price p, Quantity q, Side s) {
        trades.push_back({id, p, q, s});
    }
    void on_book_clear() { ++clears; }

    void reset() { level_changes.clear(); trades.clear(); clears = 0; }
};

template <class T>
concept Publisher = std::is_base_of_v<PublisherBase<T>, T>;

}
