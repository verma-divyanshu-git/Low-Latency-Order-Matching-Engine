#include "matching_engine/clock_probe_cli.hpp"

#include <charconv>
#include <cstddef>
#include <limits>

namespace matching_engine::measurement {
namespace {

[[nodiscard]] bool parse_positive(std::string_view text, std::uint32_t maximum,
                                  std::uint32_t& output) noexcept {
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > maximum) {
    return false;
  }
  output = value;
  return true;
}

} // namespace

CliError parse_probe_options(std::span<const std::string_view> arguments,
                             ProbeOptions& options) noexcept {
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    auto argument = arguments[index];
    std::string_view value;
    std::uint32_t* destination{};
    std::uint32_t maximum{};
    if (argument.starts_with("--samples=")) {
      value = argument;
      value.remove_prefix(sizeof("--samples=") - 1U);
      destination = &options.samples;
      maximum = 1'000'000;
    } else if (argument == "--samples") {
      if (++index >= arguments.size()) {
        return CliError::missing_value;
      }
      value = arguments[index];
      destination = &options.samples;
      maximum = 1'000'000;
    } else if (argument.starts_with("--calibration-ms=")) {
      value = argument;
      value.remove_prefix(sizeof("--calibration-ms=") - 1U);
      destination = &options.calibration_ms;
      maximum = 10'000;
    } else if (argument == "--calibration-ms") {
      if (++index >= arguments.size()) {
        return CliError::missing_value;
      }
      value = arguments[index];
      destination = &options.calibration_ms;
      maximum = 10'000;
    } else {
      return CliError::unknown_option;
    }
    if (!parse_positive(value, maximum, *destination)) {
      return CliError::invalid_value;
    }
  }
  return CliError::none;
}

const char* cli_error_name(CliError error) noexcept {
  switch (error) {
  case CliError::none:
    return "none";
  case CliError::unknown_option:
    return "unknown_option";
  case CliError::missing_value:
    return "missing_value";
  case CliError::invalid_value:
    return "invalid_value";
  }
  return "unknown_option";
}

} // namespace matching_engine::measurement
