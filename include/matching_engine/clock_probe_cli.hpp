#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace matching_engine::measurement {

struct ProbeOptions {
  std::uint32_t samples{10'000};
  std::uint32_t calibration_ms{10};
};

enum class CliError : std::uint8_t {
  none,
  unknown_option,
  missing_value,
  invalid_value,
};

[[nodiscard]] CliError parse_probe_options(std::span<const std::string_view> arguments,
                                           ProbeOptions& options) noexcept;
[[nodiscard]] const char* cli_error_name(CliError error) noexcept;

} // namespace matching_engine::measurement
