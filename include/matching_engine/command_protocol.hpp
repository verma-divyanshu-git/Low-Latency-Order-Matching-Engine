#ifndef MATCHING_ENGINE_COMMAND_PROTOCOL_HPP
#define MATCHING_ENGINE_COMMAND_PROTOCOL_HPP

#include "matching_engine/command.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace matching_engine {

inline constexpr std::uint8_t kCommandProtocolVersion = 1U;
inline constexpr std::size_t kEncodedCommandFrameSize = 40U;

enum class CommandFrameError : std::uint8_t {
  invalid_length,
  unsupported_version,
  noncanonical_header,
  invalid_payload,
};

[[nodiscard]] CommandFrameError
encode_command_frame(const CommandPayload& payload, std::span<std::byte> output) noexcept;

[[nodiscard]] std::expected<CommandPayload, CommandFrameError>
decode_command_frame(std::span<const std::byte> input) noexcept;

} // namespace matching_engine

#endif