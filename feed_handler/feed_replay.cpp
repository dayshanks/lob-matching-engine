// synthetic market-data source for benchmarking (c++23)
// accepts one TCP connection and streams N messages then closes

#include "feed_protocol.h"

#include <chrono>
#include <print>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

using namespace feed;

static std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::uint16_t port   = (argc > 1) ? (std::uint16_t)std::atoi(argv[1]) : 9100;
    std::uint64_t n_msgs = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 10'000'000ull;

    int srv = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {.s_addr = htonl(INADDR_ANY)},
        .sin_zero   = {},
    };
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { std::perror("bind"); return 1; }
    if (::listen(srv, 1) < 0) { std::perror("listen"); return 1; }
    std::println("feed source on :{} — waiting for client", port);

    int c = ::accept(srv, nullptr, nullptr);
    if (c < 0) { std::perror("accept"); return 1; }
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sbuf = 1 << 20;
    ::setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sbuf, sizeof(sbuf));
    std::println("client connected — streaming");

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int>           mix(0, 99);
    std::uniform_int_distribution<std::int64_t>  px(9900, 10100);
    std::uniform_int_distribution<std::uint32_t> qty(1, 1000);

    std::uint8_t  buf[64];
    std::uint64_t order_id = 1;
    std::uint32_t seq      = 1;
    bool ok = true;

    auto send_all = [&](std::size_t len) {
        std::size_t off = 0;
        while (off < len) {
            ssize_t s = ::send(c, buf + off, len - off, MSG_NOSIGNAL);
            if (s < 0) { if (errno == EINTR) continue; ok = false; return; }
            off += (std::size_t)s;
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < n_msgs && ok; ++i) {
        int r = mix(rng);
        std::uint64_t ts = now_ns();
        std::size_t len;
        if (r < 50) {
            WireAdd m{
                .h = {.len = sizeof(WireAdd), .type = MSG_ADD, .seq = seq++, .ts_ns = ts},
                .order_id  = order_id++,
                .symbol_id = 1,
                .side      = (rng() & 1) ? SIDE_BID : SIDE_ASK,
                .qty       = qty(rng),
                .price     = px(rng),
            };
            std::memcpy(buf, &m, sizeof(m)); len = sizeof(m);
        } else if (r < 85) {
            WireCancel m{
                .h = {.len = sizeof(WireCancel), .type = MSG_CANCEL, .seq = seq++, .ts_ns = ts},
                .order_id = 1 + (rng() % (order_id ? order_id : 1)),
            };
            std::memcpy(buf, &m, sizeof(m)); len = sizeof(m);
        } else if (r < 95) {
            WireMod m{
                .h = {.len = sizeof(WireMod), .type = MSG_MODIFY, .seq = seq++, .ts_ns = ts},
                .order_id = 1 + (rng() % (order_id ? order_id : 1)),
                .qty      = qty(rng),
                .price    = px(rng),
            };
            std::memcpy(buf, &m, sizeof(m)); len = sizeof(m);
        } else {
            WireTrade m{
                .h = {.len = sizeof(WireTrade), .type = MSG_TRADE, .seq = seq++, .ts_ns = ts},
                .order_id = 1 + (rng() % (order_id ? order_id : 1)),
                .qty      = qty(rng),
            };
            std::memcpy(buf, &m, sizeof(m)); len = sizeof(m);
        }
        send_all(len);
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::println("sent {} msgs in {:.2f}s ({:.2f} M msg/s)",
                 n_msgs, secs, n_msgs / secs / 1e6);
    ::shutdown(c, SHUT_WR);
    ::close(c);
    ::close(srv);
    return 0;
}
