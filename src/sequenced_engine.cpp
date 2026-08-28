#include "matching_engine/sequenced_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace matching_engine {
namespace {

[[nodiscard]] EngineEvent submit_event(const SequencedCommand& command, OrderId order_id,
                                       const SubmitResult& result) noexcept {
  return {.command_sequence = command.sequence,
          .order_id = order_id,
          .quantity = result.executed_quantity,
          .secondary_quantity = result.unfilled_quantity,
          .handle = result.resting_handle,
          .event_index = 0U,
          .type = EngineEventType::submit_result,
          .reason = static_cast<std::uint8_t>(result.reject_reason)};
}

} // namespace

SequencedEngine::SequencedEngine(PriceDomain domain, std::size_t max_orders,
                                 Quantity max_order_quantity, Sequence next_sequence,
                 std::uint64_t last_logical_time,
                 SelfTradePolicy self_trade_policy)
  : order_book_{domain, max_orders, max_order_quantity, self_trade_policy},
      trade_scratch_{max_orders == 0U ? nullptr : std::make_unique<Trade[]>(max_orders)},
      trade_capacity_{max_orders}, next_sequence_{next_sequence.value()},
      last_logical_time_{last_logical_time} {
  if (next_sequence.value() == 0U) {
    throw std::invalid_argument{"initial sequence must be nonzero"};
  }
}

std::size_t SequencedEngine::required_event_capacity(const CommandPayload& payload) const noexcept {
  switch (payload.tag) {
  case CommandType::submit_limit:
    return order_book_.preflight_limit(payload.side, Price{payload.price_ticks},
                                       Quantity{payload.quantity},
                                       payload.time_in_force) == RejectReason::none
               ? trade_capacity_ + 1U
               : 1U;
  case CommandType::submit_market:
    return order_book_.preflight_market(payload.side, Quantity{payload.quantity}) ==
                   RejectReason::none
               ? trade_capacity_ + 1U
               : 1U;
  case CommandType::submit_iceberg:
    return order_book_.preflight_limit(payload.side, Price{payload.price_ticks},
                                       Quantity{payload.quantity}, TimeInForce::gtc) ==
                   RejectReason::none
               ? trade_capacity_ + 1U
               : 1U;
  case CommandType::replace:
    if (order_book_.preflight_replace(Handle{payload.handle_index, payload.handle_generation},
                                      Price{payload.price_ticks},
                                      Quantity{payload.quantity}) != RejectReason::none) {
      return 1U;
    }
    return std::max<std::size_t>(trade_capacity_, 1U);
  case CommandType::cancel:
  case CommandType::amend_quantity:
    return 1U;
  }
  return 0U;
}

void SequencedEngine::write_submit_events(const SequencedCommand& command,
                                          const SubmitResult& result,
                                          std::span<EngineEvent> events) noexcept {
  events[0] = submit_event(command, OrderId{command.payload.order_id}, result);
  for (std::uint32_t index = 0U; index < result.trade_count; ++index) {
    events[static_cast<std::size_t>(index) + 1U] =
        EngineEvent::trade(command.sequence, index + 1U, trade_scratch_[index]);
  }
}

ApplyResult SequencedEngine::apply(const SequencedCommand& command,
                                   std::span<EngineEvent> events) noexcept {
  if (sequence_exhausted_) {
    return {ApplyStatus::sequence_exhausted, 0U};
  }
  if (validate_command_payload(command.payload) != CommandValidationError::none) {
    return {ApplyStatus::invalid_command, 0U};
  }
  if (command.sequence.value() != next_sequence_) {
    return {ApplyStatus::invalid_sequence, 0U};
  }
  if (command.logical_time < last_logical_time_) {
    return {ApplyStatus::decreasing_logical_time, 0U};
  }
  const std::size_t required = required_event_capacity(command.payload);
  if (events.size() < required) {
    return {ApplyStatus::insufficient_event_capacity, 0U};
  }

  const CommandPayload& payload = command.payload;
  const std::span<Trade> trades{trade_scratch_.get(), trade_capacity_};
  std::size_t event_count = 1U;
  switch (payload.tag) {
  case CommandType::submit_limit: {
    const SubmitResult result = order_book_.submit_limit(
        OrderId{payload.order_id}, payload.side, Price{payload.price_ticks},
        Quantity{payload.quantity}, payload.time_in_force, trades);
    write_submit_events(command, result, events);
    event_count += result.trade_count;
    break;
  }
  case CommandType::submit_market: {
    const SubmitResult result = order_book_.submit_market(OrderId{payload.order_id}, payload.side,
                                                          Quantity{payload.quantity}, trades);
    write_submit_events(command, result, events);
    event_count += result.trade_count;
    break;
  }
  case CommandType::submit_iceberg: {
    const SubmitResult result = order_book_.submit_iceberg(
        OrderId{payload.order_id}, payload.side, Price{payload.price_ticks},
        Quantity{payload.quantity}, payload.iceberg_display_quantity(), trades);
    write_submit_events(command, result, events);
    event_count += result.trade_count;
    break;
  }
  case CommandType::cancel: {
    const Handle handle{payload.handle_index, payload.handle_generation};
    const CancelResult result = order_book_.cancel(handle);
    events[0] = {.command_sequence = command.sequence,
                 .order_id = result.order_id,
                 .quantity = result.canceled_quantity,
                 .handle = handle,
                 .event_index = 0U,
                 .type = EngineEventType::cancel_result,
                 .reason = static_cast<std::uint8_t>(result.reject_reason)};
    break;
  }
  case CommandType::amend_quantity: {
    const Handle handle{payload.handle_index, payload.handle_generation};
    const AmendResult result = order_book_.amend_quantity(handle, Quantity{payload.quantity});
    events[0] = {.command_sequence = command.sequence,
                 .order_id = result.order_id,
                 .quantity = result.previous_quantity,
                 .secondary_quantity = result.new_quantity,
                 .handle = result.handle,
                 .event_index = 0U,
                 .type = EngineEventType::amend_result,
                 .reason = static_cast<std::uint8_t>(result.reject_reason)};
    break;
  }
  case CommandType::replace: {
    const Handle handle{payload.handle_index, payload.handle_generation};
    const auto previous = order_book_.order_info(handle);
    const SubmitResult result =
        order_book_.replace(handle, Price{payload.price_ticks}, Quantity{payload.quantity}, trades);
    write_submit_events(command, result, events);
    if (previous.has_value()) {
      events[0].order_id = previous->id;
    }
    event_count += result.trade_count;
    break;
  }
  }
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    sequence_exhausted_ = true;
  } else {
    ++next_sequence_;
  }
  last_logical_time_ = command.logical_time;
  return {ApplyStatus::applied, event_count};
}

} // namespace matching_engine
