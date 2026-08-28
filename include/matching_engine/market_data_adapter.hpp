#ifndef MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP
#define MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP

#include "matching_engine/command.hpp"
#include "matching_engine/gateway.hpp"
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
  explicit MarketDataAdapter(GatewayValidator gateway) noexcept : gateway_{std::move(gateway)} {}

  [[nodiscard]] std::expected<SequencedCommand, MarketDataAdaptError>
  adapt(const MarketDataMessage& message) noexcept;
  [[nodiscard]] GatewayRejectReason last_gateway_reject_reason() const noexcept {
    return last_gateway_reject_reason_;
  }

private:
  GatewayValidator gateway_;
  Sequencer sequencer_{};
  std::uint64_t previous_market_data_sequence_{};
  GatewayRejectReason last_gateway_reject_reason_{GatewayRejectReason::none};
};

} // namespace matching_engine

#endif