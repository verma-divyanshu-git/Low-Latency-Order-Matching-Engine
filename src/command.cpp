#include "matching_engine/command.hpp"

#include <bit>
#include <limits>

namespace matching_engine {
namespace {

[[nodiscard]] constexpr bool is_zero_handle(const CommandPayload& payload) noexcept {
  return payload.handle_index == 0U && payload.handle_generation == 0U;
}

[[nodiscard]] constexpr bool has_zero_common_unused(const CommandPayload& payload) noexcept {
  return payload.side == Side::buy && payload.time_in_force == TimeInForce::gtc &&
         payload.order_id == 0U;
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4U; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] CommandCodecError codec_error(CommandValidationError error) noexcept {
  switch (error) {
  case CommandValidationError::none:
    return CommandCodecError::none;
  case CommandValidationError::invalid_tag:
    return CommandCodecError::invalid_tag;
  case CommandValidationError::invalid_side:
    return CommandCodecError::invalid_side;
  case CommandValidationError::invalid_time_in_force:
    return CommandCodecError::invalid_time_in_force;
  case CommandValidationError::noncanonical:
    return CommandCodecError::noncanonical;
  }
  return CommandCodecError::noncanonical;
}

} // namespace

CommandValidationError validate_command_payload(const CommandPayload& payload) noexcept {
  if (payload.tag != CommandType::submit_limit && payload.tag != CommandType::submit_market &&
      payload.tag != CommandType::cancel && payload.tag != CommandType::amend_quantity &&
      payload.tag != CommandType::replace && payload.tag != CommandType::submit_iceberg &&
      payload.tag != CommandType::submit_stop && payload.tag != CommandType::submit_stop_limit) {
    return CommandValidationError::invalid_tag;
  }
  if (payload.reserved != 0U) {
    return CommandValidationError::noncanonical;
  }
  if (!is_valid_side(payload.side)) {
    return CommandValidationError::invalid_side;
  }
  if (!is_valid_time_in_force(payload.time_in_force)) {
    return CommandValidationError::invalid_time_in_force;
  }
  switch (payload.tag) {
  case CommandType::submit_limit:
    return is_zero_handle(payload) ? CommandValidationError::none
                                   : CommandValidationError::noncanonical;
  case CommandType::submit_iceberg:
    return payload.time_in_force == TimeInForce::gtc ? CommandValidationError::none
                                                      : CommandValidationError::noncanonical;
  case CommandType::submit_stop:
    return payload.time_in_force == TimeInForce::gtc && payload.price_ticks == 0
               ? CommandValidationError::none
               : CommandValidationError::noncanonical;
  case CommandType::submit_stop_limit:
    return payload.time_in_force == TimeInForce::gtc ? CommandValidationError::none
                                                      : CommandValidationError::noncanonical;
  case CommandType::submit_market:
    if (payload.time_in_force != TimeInForce::gtc || payload.price_ticks != 0 ||
        !is_zero_handle(payload)) {
      return CommandValidationError::noncanonical;
    }
    return CommandValidationError::none;
  case CommandType::cancel:
    return has_zero_common_unused(payload) && payload.price_ticks == 0 && payload.quantity == 0U
               ? CommandValidationError::none
               : CommandValidationError::noncanonical;
  case CommandType::amend_quantity:
    return has_zero_common_unused(payload) && payload.price_ticks == 0
               ? CommandValidationError::none
               : CommandValidationError::noncanonical;
  case CommandType::replace:
    return has_zero_common_unused(payload) ? CommandValidationError::none
                                           : CommandValidationError::noncanonical;
  }
  return CommandValidationError::invalid_tag;
}

CommandCodecError encode_command_payload(const CommandPayload& payload,
                                         std::span<std::byte> output) noexcept {
  if (output.size() != kEncodedCommandPayloadSize) {
    return CommandCodecError::invalid_length;
  }
  const CommandValidationError validation = validate_command_payload(payload);
  if (validation != CommandValidationError::none) {
    return codec_error(validation);
  }
  output[0] = static_cast<std::byte>(payload.tag);
  output[1] = static_cast<std::byte>(payload.side);
  output[2] = static_cast<std::byte>(payload.time_in_force);
  output[3] = std::byte{0};
  write_u64(output, 4U, payload.order_id);
  write_u64(output, 12U, std::bit_cast<std::uint64_t>(payload.price_ticks));
  write_u64(output, 20U, payload.quantity);
  write_u32(output, 28U, payload.handle_index);
  write_u32(output, 32U, payload.handle_generation);
  return CommandCodecError::none;
}

std::expected<CommandPayload, CommandCodecError>
decode_command_payload(std::span<const std::byte> input) noexcept {
  if (input.size() != kEncodedCommandPayloadSize) {
    return std::unexpected{CommandCodecError::invalid_length};
  }
  CommandPayload payload{.tag = static_cast<CommandType>(std::to_integer<std::uint8_t>(input[0])),
                         .side = static_cast<Side>(std::to_integer<std::uint8_t>(input[1])),
                         .time_in_force =
                             static_cast<TimeInForce>(std::to_integer<std::uint8_t>(input[2])),
                         .reserved = std::to_integer<std::uint8_t>(input[3]),
                         .order_id = read_u64(input, 4U),
                         .price_ticks = std::bit_cast<std::int64_t>(read_u64(input, 12U)),
                         .quantity = read_u64(input, 20U),
                         .handle_index = read_u32(input, 28U),
                         .handle_generation = read_u32(input, 32U)};
  const CommandCodecError validation = codec_error(validate_command_payload(payload));
  if (validation != CommandCodecError::none) {
    return std::unexpected{validation};
  }
  return payload;
}

std::expected<SequencedCommand, SequencerError>
Sequencer::stamp(const CommandPayload& payload, std::uint64_t logical_time) noexcept {
  if (validate_command_payload(payload) != CommandValidationError::none) {
    return std::unexpected{SequencerError::invalid_payload};
  }
  if (logical_time < last_logical_time_) {
    return std::unexpected{SequencerError::decreasing_logical_time};
  }
  if (exhausted_) {
    return std::unexpected{SequencerError::sequence_exhausted};
  }
  const SequencedCommand result{
      .payload = payload, .sequence = next_sequence_, .logical_time = logical_time};
  if (next_sequence_.value() == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
  } else {
    next_sequence_ = Sequence{next_sequence_.value() + 1U};
  }
  last_logical_time_ = logical_time;
  return result;
}

} // namespace matching_engine
