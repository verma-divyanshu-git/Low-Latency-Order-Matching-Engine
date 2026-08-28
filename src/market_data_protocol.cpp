#include "matching_engine/market_data_protocol.hpp"

#include <algorithm>
#include <type_traits>

namespace matching_engine {
namespace {

constexpr std::size_t kVersionOffset = 0U;
constexpr std::size_t kTypeOffset = 1U;
constexpr std::size_t kSideOffset = 2U;
constexpr std::size_t kSequenceOffset = 8U;
constexpr std::size_t kOrderIdOffset = 16U;
constexpr std::size_t kSecondaryOrderIdOffset = 24U;
constexpr std::size_t kPriceOffset = 32U;
constexpr std::size_t kQuantityOffset = 40U;
constexpr std::size_t kSecondaryQuantityOffset = 48U;
constexpr std::size_t kOrderCountOffset = 56U;

template <typename Integer>
void write_little_endian(Integer value, std::span<std::byte> output) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  const Unsigned unsigned_value = static_cast<Unsigned>(value);
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output[index] = static_cast<std::byte>((unsigned_value >> (index * 8U)) & 0xffU);
  }
}

template <typename Integer>
Integer read_little_endian(std::span<const std::byte> input) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned value{};
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    value |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(input[index])) << (index * 8U);
  }
  return static_cast<Integer>(value);
}

[[nodiscard]] constexpr bool is_valid_type(MarketDataMessageType type) noexcept {
  return type == MarketDataMessageType::add_order || type == MarketDataMessageType::replace_order ||
         type == MarketDataMessageType::delete_order || type == MarketDataMessageType::trade ||
         type == MarketDataMessageType::level_update;
}

[[nodiscard]] constexpr bool has_buy_side_sentinel(const MarketDataMessage& message) noexcept {
  return message.side == Side::buy;
}

[[nodiscard]] constexpr bool is_canonical(const MarketDataMessage& message) noexcept {
  switch (message.type) {
  case MarketDataMessageType::add_order:
    return is_valid_side(message.side) && message.secondary_order_id == OrderId{0U} &&
           message.secondary_quantity == Quantity{0U} && message.order_count == 0U;
  case MarketDataMessageType::replace_order:
    return has_buy_side_sentinel(message) && message.secondary_order_id == OrderId{0U} &&
           message.price == Price{0} && message.secondary_quantity == Quantity{0U} &&
           message.order_count == 0U;
  case MarketDataMessageType::delete_order:
    return has_buy_side_sentinel(message) && message.secondary_order_id == OrderId{0U} &&
           message.price == Price{0} && message.quantity == Quantity{0U} &&
           message.secondary_quantity == Quantity{0U} && message.order_count == 0U;
  case MarketDataMessageType::trade:
    return has_buy_side_sentinel(message) && message.secondary_quantity == Quantity{0U} &&
           message.order_count == 0U;
  case MarketDataMessageType::level_update:
    return is_valid_side(message.side) && message.order_id == OrderId{0U} &&
           message.secondary_order_id == OrderId{0U} && message.secondary_quantity == Quantity{0U};
  }
  return false;
}

[[nodiscard]] bool is_zero(std::span<const std::byte> input) noexcept {
  return std::ranges::all_of(input, [](std::byte value) { return value == std::byte{}; });
}

} // namespace

MarketDataFrameError encode_market_data_frame(const MarketDataMessage& message,
                                              std::span<std::byte> output) noexcept {
  if (output.size() != kEncodedMarketDataFrameSize) {
    return MarketDataFrameError::invalid_length;
  }
  if (!is_valid_type(message.type)) {
    return MarketDataFrameError::invalid_type;
  }
  if (!is_canonical(message)) {
    return (message.type == MarketDataMessageType::add_order ||
        message.type == MarketDataMessageType::level_update) &&
             !is_valid_side(message.side)
           ? MarketDataFrameError::invalid_side
           : MarketDataFrameError::noncanonical;
  }

  std::fill(output.begin(), output.end(), std::byte{});
  output[kVersionOffset] = static_cast<std::byte>(kMarketDataProtocolVersion);
  output[kTypeOffset] = static_cast<std::byte>(message.type);
  output[kSideOffset] = static_cast<std::byte>(message.side);
  write_little_endian(message.sequence, output.subspan(kSequenceOffset));
  write_little_endian(message.order_id.value(), output.subspan(kOrderIdOffset));
  write_little_endian(message.secondary_order_id.value(), output.subspan(kSecondaryOrderIdOffset));
  write_little_endian(message.price.ticks(), output.subspan(kPriceOffset));
  write_little_endian(message.quantity.value(), output.subspan(kQuantityOffset));
  write_little_endian(message.secondary_quantity.value(), output.subspan(kSecondaryQuantityOffset));
  write_little_endian(message.order_count, output.subspan(kOrderCountOffset));
  return MarketDataFrameError::none;
}

std::expected<MarketDataMessage, MarketDataFrameError>
decode_market_data_frame(std::span<const std::byte> input) noexcept {
  if (input.size() != kEncodedMarketDataFrameSize) {
    return std::unexpected{MarketDataFrameError::invalid_length};
  }
  if (std::to_integer<std::uint8_t>(input[kVersionOffset]) != kMarketDataProtocolVersion) {
    return std::unexpected{MarketDataFrameError::unsupported_version};
  }
  const auto type = static_cast<MarketDataMessageType>(std::to_integer<std::uint8_t>(input[kTypeOffset]));
  if (!is_valid_type(type)) {
    return std::unexpected{MarketDataFrameError::invalid_type};
  }
  if (!is_zero(input.subspan(3U, 5U)) || !is_zero(input.subspan(60U, 4U))) {
    return std::unexpected{MarketDataFrameError::noncanonical};
  }
  const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(input[kSideOffset]));
  const MarketDataMessage message{
      .sequence = read_little_endian<std::uint64_t>(input.subspan(kSequenceOffset)),
      .order_id = OrderId{read_little_endian<std::uint64_t>(input.subspan(kOrderIdOffset))},
      .secondary_order_id = OrderId{read_little_endian<std::uint64_t>(input.subspan(kSecondaryOrderIdOffset))},
      .price = Price{read_little_endian<std::int64_t>(input.subspan(kPriceOffset))},
      .quantity = Quantity{read_little_endian<std::uint64_t>(input.subspan(kQuantityOffset))},
      .secondary_quantity = Quantity{read_little_endian<std::uint64_t>(input.subspan(kSecondaryQuantityOffset))},
      .order_count = read_little_endian<std::uint32_t>(input.subspan(kOrderCountOffset)),
      .type = type,
      .side = side};
  if (!is_canonical(message)) {
    return std::unexpected{(type == MarketDataMessageType::add_order ||
                            type == MarketDataMessageType::level_update) && !is_valid_side(side)
                               ? MarketDataFrameError::invalid_side
                               : MarketDataFrameError::noncanonical};
  }
  return message;
}

} // namespace matching_engine