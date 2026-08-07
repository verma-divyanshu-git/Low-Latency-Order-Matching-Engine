#ifndef MATCHING_ENGINE_ORDER_HPP
#define MATCHING_ENGINE_ORDER_HPP

#include "matching_engine/types.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

namespace matching_engine {

inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kOrderLevelMask = 0x7fff'ffffU;
inline constexpr std::uint32_t kOrderSideMask = 0x8000'0000U;

namespace detail {

[[nodiscard]] constexpr std::uint32_t encode_level_side(std::uint32_t level, Side side) noexcept {
  return level | (side == Side::sell ? kOrderSideMask : 0U);
}

[[nodiscard]] constexpr std::uint32_t decode_level(std::uint32_t encoded) noexcept {
  return encoded & kOrderLevelMask;
}

[[nodiscard]] constexpr Side decode_side(std::uint32_t encoded) noexcept {
  return (encoded & kOrderSideMask) == 0U ? Side::buy : Side::sell;
}

} // namespace detail

struct Order {
  OrderId id;
  Quantity remaining;
  std::uint32_t prev_index;
  std::uint32_t next_index;

  // Bits 0-30 encode a level index in [0, kOrderLevelMask].
  // Bit 31 encodes Side, with zero for buy and one for sell.
  std::uint32_t encoded_level_side;

  // Reserved for future order flags and must be explicitly initialized by callers.
  std::uint32_t reserved_flags;
};

static_assert(sizeof(Order) == 32);
static_assert(std::is_trivially_copyable_v<Order>);

} // namespace matching_engine

#endif
