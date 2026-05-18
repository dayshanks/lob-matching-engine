#include "lob/order_book.h"
#include "lob/publisher.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Op {
    enum class Kind { Limit, Market, Cancel } kind;
    OrderId   id;
    Side      side;
    Price     price;
    Quantity  qty;
};

static std::vector<Op> generate(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<OrderId> live;
    OrderId next = 1;
    const Price mid = 10000;

    for (size_t i = 0; i < n; ++i) {
        int roll = rng() % 100;
        if (roll < 60 || live.empty()) {
            Side  s   = (rng() & 1) ? Side::Buy : Side::Sell;
            int   off = (int)(rng() % 21) - 10;
            Price p   = mid + off;
            Quantity q = 1 + (rng() % 50);
            ops.push_back({Op::Kind::Limit, next, s, p, q});
            live.push_back(next++);
        } else if (roll < 75) {
            Side s     = (rng() & 1) ? Side::Buy : Side::Sell;
            Quantity q = 1 + (rng() % 20);
            ops.push_back({Op::Kind::Market, next++, s, 0, q});
        } else {
            size_t idx = rng() % live.size();
            ops.push_back({Op::Kind::Cancel, live[idx], Side::Buy, 0, 0});
            live[idx] = live.back();
            live.pop_back();
        }
    }
    return ops;
}

int main(int argc, char** argv) {
    const size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const size_t warmup = std::min<size_t>(100'000, n / 10);

    std::printf("generating %zu ops (%zu warmup)...\n", n, warmup);
    auto ops = generate(n + warmup, 0xC0FFEE);

    OrderBook<NullPublisher> book(1 << 22);

    for (size_t i = 0; i < warmup; ++i) {
        const Op& o = ops[i];
        switch (o.kind) {
            case Op::Kind::Limit:  book.submit_limit (o.id, 0, o.side, o.price, o.qty, i); break;
            case Op::Kind::Market: book.submit_market(o.id, 0, o.side,          o.qty, i); break;
            case Op::Kind::Cancel: (void)book.cancel(o.id);                                break;
        }
    }

    std::vector<uint64_t> latencies;
    latencies.reserve(n / 10 + 1);

    auto t0 = Clock::now();
    for (size_t i = warmup; i < ops.size(); ++i) {
        const Op& o = ops[i];
        const bool sample = (i % 10) == 0;
        auto a = sample ? Clock::now() : Clock::time_point{};
        switch (o.kind) {
            case Op::Kind::Limit:  book.submit_limit (o.id, 0, o.side, o.price, o.qty, i); break;
            case Op::Kind::Market: book.submit_market(o.id, 0, o.side,          o.qty, i); break;
            case Op::Kind::Cancel: (void)book.cancel(o.id);                                break;
        }
        if (sample) {
            auto b = Clock::now();
            latencies.push_back((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
    }
    auto t1 = Clock::now();

    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) { return latencies[(size_t)(p * (latencies.size() - 1))]; };

    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double tput      = n / elapsed_s;

    std::printf("\n--- results ---\n");
    std::printf("ops:          %zu\n", n);
    std::printf("elapsed:      %.3f s\n", elapsed_s);
    std::printf("throughput:   %.0f ops/sec  (%.2fM/s)\n", tput, tput / 1e6);
    std::printf("open orders:  %zu\n", book.open_orders());
    std::printf("latency ns:   p50=%" PRIu64 "  p90=%" PRIu64 "  p99=%" PRIu64 "  p999=%" PRIu64 "  max=%" PRIu64 "\n",
                pct(0.50), pct(0.90), pct(0.99), pct(0.999), latencies.back());
    std::printf("latency us:   p50=%.2f  p90=%.2f  p99=%.2f\n",
                pct(0.50) / 1000.0, pct(0.90) / 1000.0, pct(0.99) / 1000.0);
    return 0;
}