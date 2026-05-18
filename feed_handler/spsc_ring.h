#pragma once
// lock-free SPSC ring — single producer, single consumer.
// head and tail live on separate cache lines to kill false sharing
#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

template <typename T, std::size_t N>
    requires std::is_trivially_copyable_v<T> && (N > 0) && ((N & (N - 1)) == 0)
struct SpscRing {
    static constexpr std::size_t CACHE_LINE = 64;

    alignas(CACHE_LINE) std::atomic<std::size_t> head{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> tail{0};
    alignas(CACHE_LINE) std::array<T, N> slots{};

    bool try_push(const T& v) noexcept {
        // only producer writes head — relaxed is fine
        std::size_t h = head.load(std::memory_order_relaxed);
        // acquire pairs with consumer's release-store on tail
        std::size_t t = tail.load(std::memory_order_acquire);
        if (h - t >= N) [[unlikely]] return false;
        slots[h & (N - 1)] = v;
        // release publishes the slot write to the consumer
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        std::size_t t = tail.load(std::memory_order_relaxed);
        // acquire pairs with producer's release-store on head —
        // makes the slot write visible before we read it
        std::size_t h = head.load(std::memory_order_acquire);
        if (h == t) [[unlikely]] return false;
        out = slots[t & (N - 1)];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    std::size_t size_approx() const noexcept {
        return head.load(std::memory_order_relaxed)
             - tail.load(std::memory_order_relaxed);
    }
};
