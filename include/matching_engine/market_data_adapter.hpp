#ifndef MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP
#define MATCHING_ENGINE_MARKET_DATA_ADAPTER_HPP

#include "matching_engine/command.hpp"
#include "matching_engine/gateway.hpp"
#include "matching_engine/market_data_protocol.hpp"
#include "matching_engine/sequenced_engine.hpp"

#include <expected>
#include <vector>

namespace matching_engine {

enum class MarketDataAdaptError : std::uint8_t {
  unsupported_message,
  invalid_command,
  sequence_mismatch,
  sequence_exhausted,
  unknown_order,
};

class MarketDataAdapter {
public:
  explicit MarketDataAdapter(GatewayValidator gateway) noexcept : gateway_{std::move(gateway)} {
    handles_.reserve(gateway_.max_active_orders());
  }

  [[nodiscard]] std::expected<SequencedCommand, MarketDataAdaptError>
  adapt(const MarketDataMessage& message) noexcept;
  void record_applied_event(const EngineEvent& event) noexcept;
  [[nodiscard]] GatewayRejectReason last_gateway_reject_reason() const noexcept {
    return last_gateway_reject_reason_;
  }

private:
  struct ActiveOrder {
    OrderId id{0U};
    Handle handle{};
  };

  [[nodiscard]] Handle* find_handle(OrderId id) noexcept;

  GatewayValidator gateway_;
  Sequencer sequencer_{};
  std::uint64_t previous_market_data_sequence_{};
  GatewayRejectReason last_gateway_reject_reason_{GatewayRejectReason::none};
  std::vector<ActiveOrder> handles_{};
};

} // namespace matching_engine

#endif