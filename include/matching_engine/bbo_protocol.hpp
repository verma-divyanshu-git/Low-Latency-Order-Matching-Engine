#ifndef MATCHING_ENGINE_BBO_PROTOCOL_HPP
#define MATCHING_ENGINE_BBO_PROTOCOL_HPP

#include "matching_engine/bbo.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace matching_engine {

inline constexpr std::uint8_t kBboProtocolVersion = 1U;
inline constexpr std::size_t kEncodedBboFrameSize = 40U;

enum class BboFrameError : std::uint8_t {
  invalid_length,
  unsupported_version,
  noncanonical,
};

[[nodiscard]] BboFrameError encode_bbo_frame(const BboState& state,
                                              std::span<std::byte> output) noexcept;

[[nodiscard]] std::expected<BboState, BboFrameError>
decode_bbo_frame(std::span<const std::byte> input) noexcept;

} // namespace matching_engine

#endif