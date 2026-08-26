#ifndef MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP
#define MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP

#include "matching_engine/command.hpp"
#include "matching_engine/market_data_protocol.hpp"

#include <expected>

namespace matching_engine {

enum class MarketDataAdaptError : std::uint8_t {
  unsupported_message,
  invalid_command,
  sequence_mismatch,
  sequence_exhausted,
};

class MarketDataAdapter {
public:
  [[nodiscard]] std::expected<SequencedCommand, MarketDataAdaptError>
  adapt(const MarketDataMessage& message) noexcept;

private:
  Sequencer sequencer_{};
  std::uint64_t previous_market_data_sequence_{};
};

} // namespace matching_engine

#endif