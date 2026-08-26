#include "matching_engine/command_protocol.hpp"

namespace matching_engine {

CommandFrameError encode_command_frame(const CommandPayload& payload,
                                       std::span<std::byte> output) noexcept {
  if (output.size() != kEncodedCommandFrameSize) {
    return CommandFrameError::invalid_length;
  }
  output[0] = static_cast<std::byte>(kCommandProtocolVersion);
  output[1] = std::byte{};
  output[2] = std::byte{};
  output[3] = std::byte{};
  const CommandCodecError error = encode_command_payload(payload, output.subspan(4U));
  return error == CommandCodecError::none ? CommandFrameError{} : CommandFrameError::invalid_payload;
}

std::expected<CommandPayload, CommandFrameError>
decode_command_frame(std::span<const std::byte> input) noexcept {
  if (input.size() != kEncodedCommandFrameSize) {
    return std::unexpected{CommandFrameError::invalid_length};
  }
  if (std::to_integer<std::uint8_t>(input[0]) != kCommandProtocolVersion) {
    return std::unexpected{CommandFrameError::unsupported_version};
  }
  if (input[1] != std::byte{} || input[2] != std::byte{} || input[3] != std::byte{}) {
    return std::unexpected{CommandFrameError::noncanonical_header};
  }
  const auto payload = decode_command_payload(input.subspan(4U));
  if (!payload.has_value()) {
    return std::unexpected{CommandFrameError::invalid_payload};
  }
  return *payload;
}

} // namespace matching_engine