#ifndef MATCHING_ENGINE_MARKET_DATA_PROTOCOL_HPP
#define MATCHING_ENGINE_MARKET_DATA_PROTOCOL_HPP

#include "matching_engine/types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace matching_engine {

inline constexpr std::uint8_t kMarketDataProtocolVersion = 1U;
inline constexpr std::size_t kEncodedMarketDataFrameSize = 64U;

enum class MarketDataMessageType : std::uint8_t {
  add_order = 1U,
  replace_order = 2U,
  delete_order = 3U,
  trade = 4U,
  level_update = 5U,
};

struct MarketDataMessage {
  std::uint64_t sequence{};
  OrderId order_id{0U};
  OrderId secondary_order_id{0U};
  Price price{0};
  Quantity quantity{0U};
  Quantity secondary_quantity{0U};
  std::uint32_t order_count{};
  MarketDataMessageType type{MarketDataMessageType::add_order};
  Side side{Side::buy};

  constexpr bool operator==(const MarketDataMessage&) const noexcept = default;
};

enum class MarketDataFrameError : std::uint8_t {
  none,
  invalid_length,
  unsupported_version,
  invalid_type,
  invalid_side,
  noncanonical,
  sequence_gap,
};

[[nodiscard]] MarketDataFrameError encode_market_data_frame(
    const MarketDataMessage& message, std::span<std::byte> output) noexcept;

[[nodiscard]] std::expected<MarketDataMessage, MarketDataFrameError>
decode_market_data_frame(std::span<const std::byte> input) noexcept;

[[nodiscard]] constexpr MarketDataFrameError
validate_market_data_sequence(std::uint64_t previous_sequence,
                              const MarketDataMessage& message) noexcept {
  return previous_sequence != 0U &&
                 (previous_sequence == UINT64_MAX || message.sequence != previous_sequence + 1U)
             ? MarketDataFrameError::sequence_gap
             : MarketDataFrameError::none;
}

} // namespace matching_engine

#endif