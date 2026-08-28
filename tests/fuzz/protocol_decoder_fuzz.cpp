#include "matching_engine/bbo_protocol.hpp"
#include "matching_engine/command_protocol.hpp"
#include "matching_engine/market_data_protocol.hpp"
#include "matching_engine/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

void fuzz_command(std::span<const std::byte> input) {
  const auto payload = matching_engine::decode_command_frame(input);
  if (!payload.has_value()) {
    return;
  }
  std::array<std::byte, matching_engine::kEncodedCommandFrameSize> output{};
  if (matching_engine::encode_command_frame(*payload, output) !=
      matching_engine::CommandFrameError::none) {
    __builtin_trap();
  }
}

void fuzz_bbo(std::span<const std::byte> input) {
  const auto state = matching_engine::decode_bbo_frame(input);
  if (!state.has_value()) {
    return;
  }
  std::array<std::byte, matching_engine::kEncodedBboFrameSize> output{};
  if (matching_engine::encode_bbo_frame(*state, output) != matching_engine::BboFrameError::none) {
    __builtin_trap();
  }
}

void fuzz_market_data(std::span<const std::byte> input) {
  const auto message = matching_engine::decode_market_data_frame(input);
  if (!message.has_value()) {
    return;
  }
  std::array<std::byte, matching_engine::kEncodedMarketDataFrameSize> output{};
  if (matching_engine::encode_market_data_frame(*message, output) !=
      matching_engine::MarketDataFrameError::none) {
    __builtin_trap();
  }
}

void fuzz_event(std::span<const std::byte> input) {
  const auto event = matching_engine::decode_engine_event(input);
  if (!event.has_value()) {
    return;
  }
  std::array<std::byte, matching_engine::kEncodedEngineEventSize> output{};
  if (matching_engine::encode_engine_event(*event, output) !=
      matching_engine::EventCodecError::none) {
    __builtin_trap();
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0U) {
    return 0;
  }
  const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(data + 1U), size - 1U};
  switch (data[0] % 4U) {
  case 0U:
    fuzz_command(input);
    break;
  case 1U:
    fuzz_bbo(input);
    break;
  case 2U:
    fuzz_market_data(input);
    break;
  case 3U:
    fuzz_event(input);
    break;
  default:
    __builtin_unreachable();
  }
  return 0;
}
