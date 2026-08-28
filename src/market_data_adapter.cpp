#include "matching_engine/market_data_adapter.hpp"

namespace matching_engine {

std::expected<SequencedCommand, MarketDataAdaptError>
MarketDataAdapter::adapt(const MarketDataMessage& message) noexcept {
  if (validate_market_data_sequence(previous_market_data_sequence_, message) !=
      MarketDataFrameError::none) {
    return std::unexpected{MarketDataAdaptError::sequence_mismatch};
  }
  if (message.type != MarketDataMessageType::add_order) {
    return std::unexpected{MarketDataAdaptError::unsupported_message};
  }
  last_gateway_reject_reason_ =
      gateway_.validate(message.order_id, message.side, message.price, message.quantity,
                        message.sequence);
  if (last_gateway_reject_reason_ != GatewayRejectReason::none) {
    return std::unexpected{MarketDataAdaptError::invalid_command};
  }
  const CommandPayload payload =
      CommandPayload::submit_limit(message.order_id, message.side, message.price, message.quantity);
  const auto command = sequencer_.stamp(payload, message.sequence);
  if (!command.has_value()) {
    switch (command.error()) {
    case SequencerError::invalid_payload:
      return std::unexpected{MarketDataAdaptError::invalid_command};
    case SequencerError::decreasing_logical_time:
      return std::unexpected{MarketDataAdaptError::sequence_mismatch};
    case SequencerError::sequence_exhausted:
      return std::unexpected{MarketDataAdaptError::sequence_exhausted};
    }
  }
  previous_market_data_sequence_ = message.sequence;
  return *command;
}

} // namespace matching_engine