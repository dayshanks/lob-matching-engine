#include "lob/order_book.h"
#include "lob/publisher.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> fn; };
static std::vector<TestCase>& registry() { static std::vector<TestCase> v; return v; }

#define TEST(name)                                                      \
    static void test_##name();                                          \
    static const int reg_##name = (registry().push_back({#name, test_##name}), 0); \
    static void test_##name()

#define REQUIRE(expr) do {                                              \
    if (!(expr)) {                                                      \
        std::fprintf(stderr, "FAIL: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
        std::exit(1);                                                   \
    }                                                                   \
} while (0)

using namespace lob;

static auto lim(OrderBook<>& b, OrderId id, Side s, Price p, Quantity q, ClientId c = 0, Timestamp ts = 0) {
    return b.submit_limit(id, c, s, p, q, ts);
}
static auto mkt(OrderBook<>& b, OrderId id, Side s, Quantity q, ClientId c = 0, Timestamp ts = 0) {
    return b.submit_market(id, c, s, q, ts);
}

TEST(empty_book_has_no_best_quote) {
    OrderBook<> b;
    REQUIRE(!b.best_bid().has_value());
    REQUIRE(!b.best_ask().has_value());
    REQUIRE(b.open_orders() == 0);
}

TEST(limit_rests_when_no_cross) {
    OrderBook<> b;
    REQUIRE(lim(b, 1, Side::Buy, 100, 5).empty());
    REQUIRE(b.best_bid() == 100);
    REQUIRE(b.volume_at(Side::Buy, 100) == 5);
}

TEST(level_aggregates_volume_and_count) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 101, 3);
    lim(b, 2, Side::Sell, 101, 7);
    lim(b, 3, Side::Sell, 101, 1);
    REQUIRE(b.volume_at(Side::Sell, 101) == 11);
    REQUIRE(b.orders_at(Side::Sell, 101) == 3);
}

TEST(best_quote_tracks_top_of_book) {
    OrderBook<> b;
    lim(b, 1, Side::Buy, 99,  1);
    lim(b, 2, Side::Buy, 100, 1);
    lim(b, 3, Side::Buy, 98,  1);
    REQUIRE(b.best_bid() == 100);
    lim(b, 4, Side::Sell, 105, 1);
    lim(b, 5, Side::Sell, 103, 1);
    lim(b, 6, Side::Sell, 110, 1);
    REQUIRE(b.best_ask() == 103);
}

TEST(crossed_limit_fully_matches) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5);
    auto t = lim(b, 2, Side::Buy, 100, 5);
    REQUIRE(t.size() == 1);
    REQUIRE(t[0].price == 100 && t[0].qty == 5);
    REQUIRE(t[0].maker == 1 && t[0].taker == 2);
    REQUIRE(t[0].buyer_aggressed);
    REQUIRE(b.open_orders() == 0);
}

TEST(crossed_limit_partial_fill_rests_remainder) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 3);
    auto t = lim(b, 2, Side::Buy, 100, 10);
    REQUIRE(t.size() == 1 && t[0].qty == 3);
    REQUIRE(b.best_bid() == 100);
    REQUIRE(b.volume_at(Side::Buy, 100) == 7);
}

TEST(taker_walks_multiple_levels) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 2);
    lim(b, 2, Side::Sell, 101, 2);
    lim(b, 3, Side::Sell, 102, 2);
    auto t = lim(b, 4, Side::Buy, 102, 5);
    REQUIRE(t.size() == 3);
    REQUIRE(t[0].price == 100 && t[1].price == 101 && t[2].price == 102);
    REQUIRE(t[2].qty == 1);
    REQUIRE(b.volume_at(Side::Sell, 102) == 1);
}

TEST(price_time_priority_within_level) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5, 0, 1);
    lim(b, 2, Side::Sell, 100, 5, 0, 2);
    lim(b, 3, Side::Sell, 100, 5, 0, 3);
    auto t = lim(b, 4, Side::Buy, 100, 7);
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].maker == 1 && t[0].qty == 5);
    REQUIRE(t[1].maker == 2 && t[1].qty == 2);
    REQUIRE(b.volume_at(Side::Sell, 100) == 8);
}

TEST(non_crossing_limit_does_not_match) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 101, 5);
    REQUIRE(lim(b, 2, Side::Buy, 100, 5).empty());
    REQUIRE(b.best_bid() == 100 && b.best_ask() == 101);
}

TEST(exact_price_cross_matches) {
    OrderBook<> b;
    lim(b, 1, Side::Buy, 100, 5);
    auto t = lim(b, 2, Side::Sell, 100, 5);
    REQUIRE(t.size() == 1 && t[0].price == 100);
}

TEST(market_buy_consumes_asks) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 3);
    lim(b, 2, Side::Sell, 101, 3);
    auto t = mkt(b, 99, Side::Buy, 5);
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].qty == 3 && t[1].qty == 2);
    REQUIRE(b.volume_at(Side::Sell, 101) == 1);
}

TEST(market_with_no_liquidity_returns_no_trades) {
    OrderBook<> b;
    REQUIRE(mkt(b, 1, Side::Buy, 100).empty());
    REQUIRE(b.open_orders() == 0);
}

TEST(market_with_partial_liquidity_evaporates_remainder) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 3);
    auto t = mkt(b, 2, Side::Buy, 10);
    REQUIRE(t.size() == 1 && t[0].qty == 3);
    REQUIRE(b.open_orders() == 0);
}

TEST(cancel_resting_order_removes_from_book) {
    OrderBook<> b;
    lim(b, 1, Side::Buy, 100, 5);
    REQUIRE(b.cancel(1).has_value());
    REQUIRE(!b.best_bid().has_value());
    REQUIRE(b.open_orders() == 0);
}

TEST(cancel_unknown_id_returns_error) {
    OrderBook<> b;
    auto r = b.cancel(42);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == "order id not found");
}

TEST(cancel_one_of_many_preserves_level) {
    OrderBook<> b;
    lim(b, 1, Side::Buy, 100, 5);
    lim(b, 2, Side::Buy, 100, 3);
    lim(b, 3, Side::Buy, 100, 2);
    REQUIRE(b.cancel(2).has_value());
    REQUIRE(b.volume_at(Side::Buy, 100) == 7);
    REQUIRE(b.orders_at(Side::Buy, 100) == 2);
}

TEST(cancel_after_partial_fill_uses_remaining_qty) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 10);
    lim(b, 2, Side::Buy,  100, 3);
    REQUIRE(b.volume_at(Side::Sell, 100) == 7);
    REQUIRE(b.cancel(1).has_value());
    REQUIRE(b.volume_at(Side::Sell, 100) == 0);
}

TEST(cancel_head_then_match_promotes_next) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5);
    lim(b, 2, Side::Sell, 100, 5);
    REQUIRE(b.cancel(1).has_value());
    auto t = lim(b, 3, Side::Buy, 100, 5);
    REQUIRE(t.size() == 1 && t[0].maker == 2);
}

TEST(cancel_middle_unlinks_correctly) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 2);
    lim(b, 2, Side::Sell, 100, 2);
    lim(b, 3, Side::Sell, 100, 2);
    REQUIRE(b.cancel(2).has_value());
    auto t = lim(b, 4, Side::Buy, 100, 4);
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].maker == 1 && t[1].maker == 3);
}

TEST(self_trade_blocked_taker_rests) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5, 42);
    REQUIRE(lim(b, 2, Side::Buy, 100, 5, 42).empty());
    REQUIRE(b.best_bid() == 100);
    REQUIRE(b.volume_at(Side::Buy, 100) == 5);
}

TEST(different_clients_match_freely) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5, 1);
    REQUIRE(lim(b, 2, Side::Buy, 100, 5, 2).size() == 1);
}

TEST(publisher_records_level_new) {
    OrderBook<RecordingPublisher> b;
    lim(b, 1, Side::Buy, 100, 5);
    auto& events = b.publisher().level_changes;
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].price == 100);
    REQUIRE(events[0].old_total == 0);
    REQUIRE(events[0].new_total == 5);
    REQUIRE(events[0].side == Side::Buy);
}

TEST(publisher_records_level_change_on_add) {
    OrderBook<RecordingPublisher> b;
    lim(b, 1, Side::Buy, 100, 5);
    lim(b, 2, Side::Buy, 100, 3);
    auto& events = b.publisher().level_changes;
    REQUIRE(events.size() == 2);
    REQUIRE(events[1].old_total == 5);
    REQUIRE(events[1].new_total == 8);
}

TEST(publisher_records_trade) {
    OrderBook<RecordingPublisher> b;
    lim(b, 1, Side::Sell, 100, 5);
    lim(b, 2, Side::Buy,  100, 5);
    auto& trades = b.publisher().trades;
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].price == 100 && trades[0].qty == 5);
    REQUIRE(trades[0].aggressor == Side::Buy);
}

TEST(publisher_batches_level_change_for_walk) {
    OrderBook<RecordingPublisher> b;
    lim(b, 1, Side::Sell, 100, 2);
    lim(b, 2, Side::Sell, 100, 2);
    lim(b, 3, Side::Sell, 100, 2);
    b.publisher().reset();
    lim(b, 4, Side::Buy, 100, 6);
    auto& events = b.publisher().level_changes;
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].old_total == 6 && events[0].new_total == 0);
    REQUIRE(b.publisher().trades.size() == 3);
}

TEST(publisher_records_cancel_as_level_change) {
    OrderBook<RecordingPublisher> b;
    lim(b, 1, Side::Buy, 100, 5);
    b.publisher().reset();
    REQUIRE(b.cancel(1).has_value());
    auto& events = b.publisher().level_changes;
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].old_total == 5 && events[0].new_total == 0);
}

TEST(zero_qty_order_is_rejected) {
    OrderBook<> b;
    REQUIRE(lim(b, 1, Side::Buy, 100, 0).empty());
    REQUIRE(b.open_orders() == 0);
}

TEST(duplicate_id_is_rejected) {
    OrderBook<> b;
    lim(b, 1, Side::Buy, 100, 5);
    lim(b, 1, Side::Buy, 101, 5);
    REQUIRE(b.open_orders() == 1);
    REQUIRE(b.volume_at(Side::Buy, 101) == 0);
}

TEST(level_removed_when_emptied_by_fills) {
    OrderBook<> b;
    lim(b, 1, Side::Sell, 100, 5);
    lim(b, 2, Side::Buy,  100, 5);
    REQUIRE(b.volume_at(Side::Sell, 100) == 0);
    REQUIRE(!b.best_ask().has_value());
}

TEST(pool_reused_after_cancel) {
    OrderBook<> b(8);
    for (int i = 0; i < 1000; ++i) {
        lim(b, i, Side::Buy, 100, 1);
        REQUIRE(b.cancel(i).has_value());
    }
    REQUIRE(b.open_orders() == 0);
}

TEST(deep_book_walk_full_consume) {
    OrderBook<> b;
    for (int i = 0; i < 100; ++i) lim(b, i, Side::Sell, 100 + i, 1);
    auto t = lim(b, 9999, Side::Buy, 199, 100);
    REQUIRE(t.size() == 100);
    REQUIRE(!b.best_ask().has_value());
}

TEST(random_sequence_keeps_invariants) {
    std::mt19937_64 rng(1234);
    OrderBook<> b;
    std::vector<OrderId> live;
    OrderId next = 1;
    for (int i = 0; i < 5000; ++i) {
        int action = rng() % 4;
        if (action < 2 || live.empty()) {
            Side s   = (rng() & 1) ? Side::Buy : Side::Sell;
            Price p  = 95 + (rng() % 11);
            Quantity q = 1 + (rng() % 10);
            lim(b, next, s, p, q);
            live.push_back(next++);
        } else {
            size_t idx = rng() % live.size();
            if (b.cancel(live[idx]).has_value()) {
                live[idx] = live.back();
                live.pop_back();
            }
        }
        REQUIRE(b.open_orders() <= live.size());
    }
}

int main() {
    int passed = 0, total = (int)registry().size();
    for (auto& tc : registry()) {
        tc.fn();
        std::printf("PASS  %s\n", tc.name);
        ++passed;
    }
    std::printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
