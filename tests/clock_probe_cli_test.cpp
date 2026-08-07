#include "matching_engine/clock_probe_cli.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using namespace std::literals;
using matching_engine::measurement::CliError;
using matching_engine::measurement::ProbeOptions;

TEST(ClockProbeCli, AcceptsBoundedPositiveValues) {
  constexpr std::array arguments{"clock_probe"sv, "--samples"sv, "4096"sv, "--calibration-ms=25"sv};
  ProbeOptions options{};
  EXPECT_EQ(matching_engine::measurement::parse_probe_options(arguments, options), CliError::none);
  EXPECT_EQ(options.samples, 4096U);
  EXPECT_EQ(options.calibration_ms, 25U);
}

TEST(ClockProbeCli, RejectsZeroMalformedTrailingAndOversizeValues) {
  constexpr std::array invalid{
      std::array{"clock_probe"sv, "--samples=0"sv},
      std::array{"clock_probe"sv, "--samples=no"sv},
      std::array{"clock_probe"sv, "--samples=10x"sv},
      std::array{"clock_probe"sv, "--samples=1000001"sv},
      std::array{"clock_probe"sv, "--calibration-ms=10001"sv},
  };

  for (const auto& arguments : invalid) {
    ProbeOptions options{};
    EXPECT_EQ(matching_engine::measurement::parse_probe_options(arguments, options),
              CliError::invalid_value);
  }
}

TEST(ClockProbeCli, RejectsUnknownAndMissingOptions) {
  constexpr std::array unknown{"clock_probe"sv, "--other=1"sv};
  constexpr std::array missing{"clock_probe"sv, "--samples"sv};
  ProbeOptions options{};
  EXPECT_EQ(matching_engine::measurement::parse_probe_options(unknown, options),
            CliError::unknown_option);
  EXPECT_EQ(matching_engine::measurement::parse_probe_options(missing, options),
            CliError::missing_value);
}

} // namespace
