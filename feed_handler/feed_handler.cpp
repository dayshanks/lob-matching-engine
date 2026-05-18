// market-data feed handler (c++23)
// producer: epoll-driven recv + parse + push to SPSC ring
// consumer: pop + dispatch to order book, record pipeline latency
// shutdown: shared stop_source; stop_callback wakes producer's epoll

#include "spsc_ring.h"
#include "feed_protocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <print>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

using namespace feed;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t RING_SLOTS = 1uz << 16;
constexpr std::size_t RECV_BUF   = 1uz << 16;

using EventRing = SpscRing<MarketEvent, RING_SLOTS>;

// stub book — replace with your matching engine's book class. concept
// BookLike (in feed_protocol.h) verifies the signatures at compile time
struct StubBook {
    std::uint64_t adds = 0, mods = 0, cancels = 0, trades = 0;
    void on_add   (std::uint64_t, std::uint8_t, std::uint32_t, std::int64_t, std::uint16_t) { ++adds; }
    void on_modify(std::uint64_t, std::uint32_t, std::int64_t)                              { ++mods; }
    void on_cancel(std::uint64_t)                                                           { ++cancels; }
    void on_trade (std::uint64_t, std::uint32_t)                                            { ++trades; }
};
static_assert(BookLike<StubBook>);

static inline std::uint64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// busy-pause hint — yields SMT execution resources, doesn't sleep
static inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

static void pin_to_cpu(std::jthread& t, int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    ::pthread_setaffinity_np(t.native_handle(), sizeof(s), &s);
}

// parse one message from a byte span — returns bytes consumed, 0 if incomplete
std::size_t parse_one(std::span<const std::byte> buf, MarketEvent& ev) {
    if (buf.size() < sizeof(WireHdr)) [[unlikely]] return 0;
    WireHdr h; std::memcpy(&h, buf.data(), sizeof(h));
    if (h.len < sizeof(WireHdr) || buf.size() < h.len) [[unlikely]] return 0;

    ev.type = h.type; ev.seq = h.seq; ev.exch_ts_ns = h.ts_ns;
    switch (h.type) {
    case MSG_ADD: {
        if (h.len < sizeof(WireAdd)) return h.len;
        WireAdd m; std::memcpy(&m, buf.data(), sizeof(m));
        ev.order_id = m.order_id; ev.symbol_id = m.symbol_id;
        ev.side = m.side; ev.qty = m.qty; ev.price = m.price;
        break;
    }
    case MSG_MODIFY: {
        if (h.len < sizeof(WireMod)) return h.len;
        WireMod m; std::memcpy(&m, buf.data(), sizeof(m));
        ev.order_id = m.order_id; ev.qty = m.qty; ev.price = m.price;
        break;
    }
    case MSG_CANCEL: {
        if (h.len < sizeof(WireCancel)) return h.len;
        WireCancel m; std::memcpy(&m, buf.data(), sizeof(m));
        ev.order_id = m.order_id;
        break;
    }
    case MSG_TRADE: {
        if (h.len < sizeof(WireTrade)) return h.len;
        WireTrade m; std::memcpy(&m, buf.data(), sizeof(m));
        ev.order_id = m.order_id; ev.qty = m.qty;
        break;
    }
    default: break;  // unknown — advance by len, don't desync the stream
    }
    return h.len;
}

void producer_loop(std::stop_token st, std::stop_source& ss,
                   int feed_fd, int wake_fd, EventRing* ring,
                   std::atomic<std::uint64_t>* msgs_in) {
    int ep = ::epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET; ev.data.fd = feed_fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, feed_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = wake_fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, wake_fd, &ev);

    std::vector<std::byte> buf(RECV_BUF);
    std::size_t used = 0;
    epoll_event events[8];
    bool eof = false;

    while (!st.stop_requested() && !eof) {
        int n = ::epoll_wait(ep, events, 8, -1);
        if (n < 0) [[unlikely]] { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == wake_fd) {
                std::uint64_t v; if (::read(wake_fd, &v, sizeof(v)) < 0) { /* drain best-effort */ }
                continue;
            }
            // drain feed socket — edge-triggered, must read until EAGAIN
            for (;;) {
                if (used == buf.size()) used = 0;          // overflow drop
                ssize_t r = ::recv(feed_fd, buf.data() + used, buf.size() - used, 0);
                if (r > 0) used += (std::size_t)r;
                else if (r == 0) { eof = true; break; }
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    eof = true; break;
                }
            }
            // parse + push everything available, memmove leftover
            std::size_t off = 0;
            while (off < used) {
                MarketEvent mev{};
                auto consumed = parse_one(std::span(buf.data() + off, used - off), mev);
                if (consumed == 0) break;
                mev.recv_ts_ns = now_ns();
                while (!ring->try_push(mev)) [[unlikely]] cpu_pause();
                msgs_in->fetch_add(1, std::memory_order_relaxed);
                off += consumed;
            }
            if (off > 0 && off < used) std::memmove(buf.data(), buf.data() + off, used - off);
            used -= off;
        }
    }

    // request_stop is release-ordered — every prior push is visible to
    // the consumer once it observes stop_requested via acquire-load
    ss.request_stop();
    ::close(ep);
}

template <BookLike Book>
void consumer_loop(std::stop_token st, EventRing* ring, Book* book,
                   std::vector<std::uint64_t>* latencies) {
    MarketEvent ev;
    auto apply = [&](const MarketEvent& e) {
        switch (e.type) {
        case MSG_ADD:    book->on_add   (e.order_id, e.side, e.qty, e.price, e.symbol_id); break;
        case MSG_MODIFY: book->on_modify(e.order_id, e.qty, e.price); break;
        case MSG_CANCEL: book->on_cancel(e.order_id); break;
        case MSG_TRADE:  book->on_trade (e.order_id, e.qty); break;
        }
        latencies->push_back(now_ns() - e.recv_ts_ns);
    };

    while (true) {
        if (ring->try_pop(ev)) { apply(ev); continue; }
        if (st.stop_requested()) [[unlikely]] {
            while (ring->try_pop(ev)) apply(ev);   // final drain
            break;
        }
        cpu_pause();
    }
}

int connect_feed(const char* host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = {},
        .sin_zero   = {},
    };
    inet_pton(AF_INET, host, &addr.sin_addr);
    int rc = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) [[unlikely]] {
        std::perror("connect"); ::close(fd); return -1;
    }
    pollfd pf{.fd = fd, .events = POLLOUT, .revents = 0};
    if (::poll(&pf, 1, 5000) <= 0) {
        std::println(stderr, "connect timeout"); ::close(fd); return -1;
    }
    int err = 0; socklen_t ln = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &ln);
    if (err) { errno = err; std::perror("connect"); ::close(fd); return -1; }
    return fd;
}

// signal-safe flag — atomic store from a handler is safe;
// stop_source::request_stop is not, so main forwards the flag
std::atomic<bool> g_signal{false};

} // namespace

int main(int argc, char** argv) {
    const char*   host = (argc > 1) ? argv[1] : "127.0.0.1";
    std::uint16_t port = (argc > 2) ? (std::uint16_t)std::atoi(argv[2]) : 9100;
    int prod_cpu       = (argc > 3) ? std::atoi(argv[3]) : 2;
    int cons_cpu       = (argc > 4) ? std::atoi(argv[4]) : 4;

    int fd = connect_feed(host, port);
    if (fd < 0) return 1;

    std::signal(SIGINT,  [](int){ g_signal.store(true, std::memory_order_release); });
    std::signal(SIGTERM, [](int){ g_signal.store(true, std::memory_order_release); });

    int wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    auto ring   = std::make_unique<EventRing>();
    StubBook book;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(50'000'000);
    std::atomic<std::uint64_t> msgs_in{0};

    auto t0 = std::chrono::steady_clock::now();
    std::stop_source ss;
    // when stop fires, write the eventfd to break the producer out of epoll_wait
    std::stop_callback wake_cb(ss.get_token(), [wake_fd]{
        std::uint64_t one = 1; if (::write(wake_fd, &one, sizeof(one)) < 0) { /* wake best-effort */ }
    });
    {
        std::jthread t_prod([&](std::stop_token st){
            producer_loop(st, ss, fd, wake_fd, ring.get(), &msgs_in);
        });
        std::jthread t_cons([&](std::stop_token st){
            consumer_loop(st, ring.get(), &book, &latencies);
        });
        pin_to_cpu(t_prod, prod_cpu);
        pin_to_cpu(t_cons, cons_cpu);

        while (!g_signal.load(std::memory_order_acquire) && !ss.stop_requested()) {
            std::this_thread::sleep_for(50ms);
        }
        if (!ss.stop_requested()) ss.request_stop();
    }   // jthread dtors: request_stop (no-op if already stopped) + join
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::ranges::sort(latencies);
    auto pct = [&](double p){
        return latencies.empty() ? 0ull
            : latencies[(std::size_t)((latencies.size() - 1) * p)];
    };

    std::println("events:        {}",                  latencies.size());
    std::println("duration:      {:.2f}s",             secs);
    std::println("throughput:    {:.2f} M msg/s",      latencies.size() / secs / 1e6);
    std::println("pipeline p50:  {} ns",               pct(0.50));
    std::println("pipeline p99:  {} ns",               pct(0.99));
    std::println("pipeline p999: {} ns",               pct(0.999));
    std::println("pipeline max:  {} ns",               latencies.empty() ? 0ull : latencies.back());
    std::println("book counts:   A={} M={} X={} T={}", book.adds, book.mods, book.cancels, book.trades);

    ::close(wake_fd);
    ::close(fd);
    return 0;
}
