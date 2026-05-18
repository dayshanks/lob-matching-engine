#pragma once
#include <cstdint>

namespace lob {

using OrderId   = uint64_t;
using ClientId  = uint64_t;
using Price     = int64_t;
using Quantity  = uint64_t;
using Timestamp = uint64_t;

enum class Side : uint8_t { Buy, Sell };

}
