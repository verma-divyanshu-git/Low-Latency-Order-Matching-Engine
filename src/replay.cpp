#include "matching_engine/replay.hpp"

#include <array>
#include <limits>

namespace matching_engine {
namespace {

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

bool valid_event_type(EngineEventType type) noexcept {
  return type == EngineEventType::submit_result || type == EngineEventType::trade ||
         type == EngineEventType::cancel_result || type == EngineEventType::amend_result ||
      type == EngineEventType::stop_triggered || type == EngineEventType::auction_result;
}

} // namespace

EventCodecError encode_engine_event(const EngineEvent& event,
                                    std::span<std::byte> output) noexcept {
  if (output.size() != kEncodedEngineEventSize) {
    return EventCodecError::invalid_length;
  }
  if (!valid_event_type(event.type)) {
    return EventCodecError::invalid_type;
  }
  if (event.reserved != 0U) {
    return EventCodecError::noncanonical;
  }
  for (std::byte& value : output) {
    value = std::byte{0};
  }
  write_u64(output, 0U, event.command_sequence.value());
  write_u64(output, 8U, event.order_id.value());
  write_u64(output, 16U, event.secondary_order_id.value());
  write_u64(output, 24U, static_cast<std::uint64_t>(event.price.ticks()));
  write_u64(output, 32U, event.quantity.value());
  write_u64(output, 40U, event.secondary_quantity.value());
  write_u32(output, 48U, event.handle.index);
  write_u32(output, 52U, event.handle.generation);
  write_u32(output, 56U, event.event_index);
  output[60U] = static_cast<std::byte>(event.type);
  output[61U] = static_cast<std::byte>(event.reason);
  return EventCodecError::none;
}

std::expected<EngineEvent, EventCodecError>
decode_engine_event(std::span<const std::byte> input) noexcept {
  if (input.size() != kEncodedEngineEventSize) {
    return std::unexpected{EventCodecError::invalid_length};
  }
  const auto type = static_cast<EngineEventType>(std::to_integer<std::uint8_t>(input[60U]));
  if (!valid_event_type(type)) {
    return std::unexpected{EventCodecError::invalid_type};
  }
  if (input[62U] != std::byte{0} || input[63U] != std::byte{0}) {
    return std::unexpected{EventCodecError::noncanonical};
  }
  return EngineEvent{.command_sequence = Sequence{read_u64(input, 0U)},
                     .order_id = OrderId{read_u64(input, 8U)},
                     .secondary_order_id = OrderId{read_u64(input, 16U)},
                     .price = Price{static_cast<std::int64_t>(read_u64(input, 24U))},
                     .quantity = Quantity{read_u64(input, 32U)},
                     .secondary_quantity = Quantity{read_u64(input, 40U)},
                     .handle = Handle{read_u32(input, 48U), read_u32(input, 52U)},
                     .event_index = read_u32(input, 56U),
                     .type = type,
                     .reason = std::to_integer<std::uint8_t>(input[61U])};
}

EventCodecError ReplayFingerprint::add(const EngineEvent& event) noexcept {
  std::array<std::byte, kEncodedEngineEventSize> bytes{};
  const EventCodecError encoded = encode_engine_event(event, bytes);
  if (encoded != EventCodecError::none) {
    return encoded;
  }
  for (const std::byte value : bytes) {
    crc_state_ ^= std::to_integer<std::uint8_t>(value);
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (crc_state_ & 1U);
      crc_state_ = (crc_state_ >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  ++event_count_;
  byte_count_ += bytes.size();
  return EventCodecError::none;
}

std::expected<ReplayResult, ReplayError>
replay_journal(MmapJournal& journal, SequencedEngine& engine, Sequence snapshot_sequence,
               std::uint64_t snapshot_logical_time, std::span<EngineEvent> event_buffer) noexcept {
  ReplayResult result{};
  const std::uint64_t snapshot_value = snapshot_sequence.value();
  const bool terminal_point = snapshot_value == std::numeric_limits<std::uint64_t>::max();
  if (terminal_point || engine.sequence_exhausted()) {
    if (terminal_point && engine.sequence_exhausted() &&
        engine.next_sequence().value() == std::numeric_limits<std::uint64_t>::max() &&
        engine.last_logical_time() == snapshot_logical_time) {
      return std::unexpected{ReplayError::unverifiable_boundary};
    }
    return std::unexpected{ReplayError::engine_state_mismatch};
  }
  if ((snapshot_value == 0U && snapshot_logical_time != 0U) ||
      engine.next_sequence().value() != snapshot_value + 1U ||
      engine.last_logical_time() != snapshot_logical_time) {
    return std::unexpected{ReplayError::engine_state_mismatch};
  }
  if (snapshot_value > journal.size()) {
    return std::unexpected{ReplayError::boundary_missing};
  }
  if (snapshot_value != 0U) {
    const auto boundary = journal.read(snapshot_value - 1U);
    if (!boundary.has_value()) {
      return std::unexpected{ReplayError::journal};
    }
    if (boundary->sequence != snapshot_sequence) {
      return std::unexpected{ReplayError::boundary_missing};
    }
    if (boundary->logical_time != snapshot_logical_time) {
      return std::unexpected{ReplayError::boundary_time_mismatch};
    }
  }
  if (snapshot_value < journal.size()) {
    const auto first_suffix = journal.read(snapshot_value);
    if (!first_suffix.has_value()) {
      return std::unexpected{ReplayError::journal};
    }
    if (first_suffix->sequence != engine.next_sequence()) {
      return std::unexpected{ReplayError::sequence_gap};
    }
  }
  if (event_buffer.size() < engine.maximum_event_capacity()) {
    return std::unexpected{ReplayError::apply};
  }
  for (std::uint64_t index = snapshot_value; index < journal.size(); ++index) {
    const auto command = journal.read(index);
    if (!command.has_value()) {
      return std::unexpected{ReplayError::journal};
    }
    if (command->sequence != engine.next_sequence()) {
      return std::unexpected{ReplayError::sequence_gap};
    }
    const ApplyResult applied = engine.apply(*command, event_buffer);
    if (applied.status != ApplyStatus::applied) {
      return std::unexpected{ReplayError::apply};
    }
    if (result.commands_applied == 0U) {
      result.first_sequence = command->sequence;
    }
    result.last_sequence = command->sequence;
    ++result.commands_applied;
    for (std::size_t event_index = 0; event_index < applied.event_count; ++event_index) {
      if (result.fingerprint.add(event_buffer[event_index]) != EventCodecError::none) {
        return std::unexpected{ReplayError::apply};
      }
    }
    if (engine.order_book().check_invariants().violation != InvariantViolation::none) {
      return std::unexpected{ReplayError::invariant};
    }
  }
  return result;
}

std::expected<ReplayResult, MarketDataReplayError>
replay_market_data(MarketDataInputStream& input, MarketDataAdapter& adapter, SequencedEngine& engine,
                   std::span<EngineEvent> event_buffer) noexcept {
  ReplayResult result{};
  if (event_buffer.size() < engine.maximum_event_capacity()) {
    return std::unexpected{MarketDataReplayError::apply};
  }

  for (;;) {
    const auto message = input.read_next();
    if (!message.has_value()) {
      return std::unexpected{MarketDataReplayError::input};
    }
    if (!message->has_value()) {
      return result;
    }
    const auto command = adapter.adapt(**message);
    if (!command.has_value()) {
      return std::unexpected{MarketDataReplayError::adapter};
    }
    const ApplyResult applied = engine.apply(*command, event_buffer);
    if (applied.status != ApplyStatus::applied) {
      return std::unexpected{MarketDataReplayError::apply};
    }
    if (result.commands_applied == 0U) {
      result.first_sequence = command->sequence;
    }
    result.last_sequence = command->sequence;
    ++result.commands_applied;
    for (std::size_t event_index = 0U; event_index < applied.event_count; ++event_index) {
      adapter.record_applied_event(event_buffer[event_index]);
      if (result.fingerprint.add(event_buffer[event_index]) != EventCodecError::none) {
        return std::unexpected{MarketDataReplayError::apply};
      }
    }
    if (engine.order_book().check_invariants().violation != InvariantViolation::none) {
      return std::unexpected{MarketDataReplayError::invariant};
    }
  }
}

} // namespace matching_engine
