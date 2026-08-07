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

enum class CompilerFamily : std::uint8_t {
  appleclang,
  clang,
  gcc,
  msvc,
  unknown,
};

struct CompilerMetadata {
  CompilerFamily family{CompilerFamily::unknown};
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};
};

[[nodiscard]] CliError parse_probe_options(std::span<const std::string_view> arguments,
                                           ProbeOptions& options) noexcept;
[[nodiscard]] const char* cli_error_name(CliError error) noexcept;
[[nodiscard]] CompilerMetadata compiler_metadata() noexcept;
[[nodiscard]] const char* compiler_family_name(CompilerFamily family) noexcept;

} // namespace matching_engine::measurement
