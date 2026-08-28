#include "matching_engine/market_data_adapter.hpp"

namespace matching_engine {

Handle* MarketDataAdapter::find_handle(OrderId id) noexcept {
  for (ActiveOrder& active : handles_) {
    if (active.id == id) {
      return &active.handle;
    }
  }
  return nullptr;
}

std::expected<SequencedCommand, MarketDataAdaptError>
MarketDataAdapter::adapt(const MarketDataMessage& message) noexcept {
  if (validate_market_data_sequence(previous_market_data_sequence_, message) !=
      MarketDataFrameError::none) {
    return std::unexpected{MarketDataAdaptError::sequence_mismatch};
  }
  CommandPayload payload;
  switch (message.type) {
  case MarketDataMessageType::add_order:
    last_gateway_reject_reason_ =
        gateway_.validate(message.order_id, message.side, message.price, message.quantity,
                          message.sequence);
    if (last_gateway_reject_reason_ != GatewayRejectReason::none) {
      return std::unexpected{MarketDataAdaptError::invalid_command};
    }
    payload = CommandPayload::submit_limit(message.order_id, message.side, message.price,
                                           message.quantity);
    break;
  case MarketDataMessageType::delete_order: {
    Handle* const handle = find_handle(message.order_id);
    if (handle == nullptr) {
      return std::unexpected{MarketDataAdaptError::unknown_order};
    }
    payload = CommandPayload::cancel(*handle);
    break;
  }
  default:
    return std::unexpected{MarketDataAdaptError::unsupported_message};
  }
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

void MarketDataAdapter::record_applied_event(const EngineEvent& event) noexcept {
  if (event.type == EngineEventType::submit_result) {
    if (event.handle.index == kInvalidIndex) {
      gateway_.release(event.order_id);
      return;
    }
    handles_.push_back({event.order_id, event.handle});
    return;
  }
  if (event.type != EngineEventType::cancel_result || event.reason != 0U) {
    return;
  }
  for (std::size_t index = 0U; index < handles_.size(); ++index) {
    if (handles_[index].id == event.order_id) {
      handles_[index] = handles_.back();
      handles_.pop_back();
      gateway_.release(event.order_id);
      return;
    }
  }
}

} // namespace matching_engine