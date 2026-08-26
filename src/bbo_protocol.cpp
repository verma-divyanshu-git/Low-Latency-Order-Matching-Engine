#include "matching_engine/bbo_protocol.hpp"

#include <algorithm>

namespace matching_engine {
namespace {

constexpr std::size_t kBidPresentOffset = 4U;
constexpr std::size_t kAskPresentOffset = 5U;
constexpr std::size_t kBidPriceOffset = 8U;
constexpr std::size_t kBidQuantityOffset = 16U;
constexpr std::size_t kAskPriceOffset = 24U;
constexpr std::size_t kAskQuantityOffset = 32U;

template <typename Integer>
void encode_little_endian(Integer value, std::span<std::byte> output) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  const Unsigned unsigned_value = static_cast<Unsigned>(value);
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output[index] = static_cast<std::byte>((unsigned_value >> (index * 8U)) & 0xffU);
  }
}

template <typename Integer>
Integer decode_little_endian(std::span<const std::byte> input) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned value{};
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    value |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(input[index])) << (index * 8U);
  }
  return static_cast<Integer>(value);
}

[[nodiscard]] bool is_zero(std::span<const std::byte> input) noexcept {
  for (const std::byte value : input) {
    if (value != std::byte{}) {
      return false;
    }
  }
  return true;
}

} // namespace

BboFrameError encode_bbo_frame(const BboState& state, std::span<std::byte> output) noexcept {
  if (output.size() != kEncodedBboFrameSize) {
    return BboFrameError::invalid_length;
  }

  std::fill(output.begin(), output.end(), std::byte{});
  output[0] = static_cast<std::byte>(kBboProtocolVersion);
  output[kBidPresentOffset] = static_cast<std::byte>(state.bid_price.has_value());
  output[kAskPresentOffset] = static_cast<std::byte>(state.ask_price.has_value());
  if (state.bid_price.has_value()) {
    encode_little_endian(state.bid_price->ticks(), output.subspan(kBidPriceOffset));
    encode_little_endian(state.bid_quantity.value(), output.subspan(kBidQuantityOffset));
  }
  if (state.ask_price.has_value()) {
    encode_little_endian(state.ask_price->ticks(), output.subspan(kAskPriceOffset));
    encode_little_endian(state.ask_quantity.value(), output.subspan(kAskQuantityOffset));
  }
  return BboFrameError::none;
}

std::expected<BboState, BboFrameError>
decode_bbo_frame(std::span<const std::byte> input) noexcept {
  if (input.size() != kEncodedBboFrameSize) {
    return std::unexpected{BboFrameError::invalid_length};
  }
  if (std::to_integer<std::uint8_t>(input[0]) != kBboProtocolVersion) {
    return std::unexpected{BboFrameError::unsupported_version};
  }
  if (!is_zero(input.subspan(1U, 3U)) || input[kBidPresentOffset] > std::byte{1} ||
      input[kAskPresentOffset] > std::byte{1} || !is_zero(input.subspan(6U, 2U))) {
    return std::unexpected{BboFrameError::noncanonical};
  }

  const bool bid_present = input[kBidPresentOffset] == std::byte{1};
  const bool ask_present = input[kAskPresentOffset] == std::byte{1};
  const auto bid_price_bytes = input.subspan(kBidPriceOffset, sizeof(std::int64_t));
  const auto bid_quantity_bytes = input.subspan(kBidQuantityOffset, sizeof(std::uint64_t));
  const auto ask_price_bytes = input.subspan(kAskPriceOffset, sizeof(std::int64_t));
  const auto ask_quantity_bytes = input.subspan(kAskQuantityOffset, sizeof(std::uint64_t));
  if ((!bid_present && (!is_zero(bid_price_bytes) || !is_zero(bid_quantity_bytes))) ||
      (!ask_present && (!is_zero(ask_price_bytes) || !is_zero(ask_quantity_bytes)))) {
    return std::unexpected{BboFrameError::noncanonical};
  }

  return BboState{.bid_price = bid_present
                                  ? std::optional<Price>{Price{decode_little_endian<std::int64_t>(bid_price_bytes)}}
                                  : std::nullopt,
                  .bid_quantity = bid_present
                                      ? Quantity{decode_little_endian<std::uint64_t>(bid_quantity_bytes)}
                                      : Quantity{0U},
                  .ask_price = ask_present
                                  ? std::optional<Price>{Price{decode_little_endian<std::int64_t>(ask_price_bytes)}}
                                  : std::nullopt,
                  .ask_quantity = ask_present
                                      ? Quantity{decode_little_endian<std::uint64_t>(ask_quantity_bytes)}
                                      : Quantity{0U}};
}

} // namespace matching_engine