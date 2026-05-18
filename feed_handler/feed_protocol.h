#pragma once
// wire format on the network + parsed MarketEvent in the ring +
// the BookLike concept your matching engine must satisfy
#include <concepts>
#include <cstdint>

namespace feed {

enum : std::uint8_t { MSG_ADD = 'A', MSG_MODIFY = 'M', MSG_CANCEL = 'X', MSG_TRADE = 'T' };
enum : std::uint8_t { SIDE_BID = 'B', SIDE_ASK = 'S' };

// wire format — packed, native LE on x86/arm64.
// length prefix frames variable-size messages on the byte stream
#pragma pack(push, 1)
struct WireHdr    { std::uint16_t len; std::uint8_t type; std::uint32_t seq; std::uint64_t ts_ns; };
struct WireAdd    { WireHdr h; std::uint64_t order_id; std::uint16_t symbol_id; std::uint8_t side; std::uint32_t qty; std::int64_t price; };
struct WireMod    { WireHdr h; std::uint64_t order_id; std::uint32_t qty; std::int64_t price; };
struct WireCancel { WireHdr h; std::uint64_t order_id; };
struct WireTrade  { WireHdr h; std::uint64_t order_id; std::uint32_t qty; };
#pragma pack(pop)

// parsed event handed through the ring. padded to one cache line so
// adjacent slots never share a line — important when producer writes
// slot N+1 while consumer reads slot N
struct alignas(64) MarketEvent {
    std::uint8_t  type;
    std::uint8_t  side;
    std::uint16_t symbol_id;
    std::uint32_t qty;
    std::int64_t  price;
    std::uint64_t order_id;
    std::uint64_t exch_ts_ns;   // from wire
    std::uint64_t recv_ts_ns;   // stamped by producer after parse
    std::uint32_t seq;
    std::uint32_t _pad0;
    std::uint8_t  _pad1[16];
};
static_assert(sizeof(MarketEvent) == 64);

// your order book plugs in by satisfying this concept — duck-typed,
// no inheritance. compile error names the missing/mismatched method
template <typename B>
concept BookLike = requires(B& b,
                            std::uint64_t oid,
                            std::uint8_t  side,
                            std::uint32_t qty,
                            std::int64_t  px,
                            std::uint16_t sym) {
    { b.on_add   (oid, side, qty, px, sym) } -> std::same_as<void>;
    { b.on_modify(oid, qty, px) }            -> std::same_as<void>;
    { b.on_cancel(oid) }                     -> std::same_as<void>;
    { b.on_trade (oid, qty) }                -> std::same_as<void>;
};

} // namespace feed
