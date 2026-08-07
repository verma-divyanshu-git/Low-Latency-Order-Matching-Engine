#include "matching_engine/clock_probe_cli.hpp"
#include "matching_engine/measurement.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>

namespace {

[[nodiscard]] constexpr std::string_view platform_name() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
  return "macos_arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
  return "macos_x86_64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux_x86_64";
#elif defined(__linux__) && defined(__aarch64__)
  return "linux_arm64";
#else
  return "other";
#endif
}

[[nodiscard]] constexpr std::string_view compiler_name() noexcept {
#if defined(__apple_build_version__)
  return "appleclang";
#elif defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#elif defined(_MSC_VER)
  return "msvc";
#else
  return "unknown";
#endif
}

[[nodiscard]] constexpr std::string_view compiler_version() noexcept {
#if defined(__clang__)
  return __clang_version__;
#elif defined(__GNUC__)
  return __VERSION__;
#elif defined(_MSC_FULL_VER)
#define MATCHING_ENGINE_STRINGIFY_IMPL(value) #value
#define MATCHING_ENGINE_STRINGIFY(value) MATCHING_ENGINE_STRINGIFY_IMPL(value)
  return MATCHING_ENGINE_STRINGIFY(_MSC_FULL_VER);
#else
  return "unknown";
#endif
}

} // namespace

int main(int argc, char** argv) {
  std::array<std::string_view, 5> argument_storage{};
  if (argc <= 0 || static_cast<std::size_t>(argc) > argument_storage.size()) {
    std::cerr << "clock_probe: invalid_value\n";
    return 2;
  }
  for (int index = 0; index < argc; ++index) {
    argument_storage[static_cast<std::size_t>(index)] = argv[index];
  }
  const std::span arguments(argument_storage.data(), static_cast<std::size_t>(argc));

  matching_engine::measurement::ProbeOptions options{};
  const auto cli_error = matching_engine::measurement::parse_probe_options(arguments, options);
  if (cli_error != matching_engine::measurement::CliError::none) {
    std::cerr << "clock_probe: " << matching_engine::measurement::cli_error_name(cli_error) << '\n';
    return 2;
  }

  const auto capabilities = matching_engine::measurement::clock_capabilities();
  const auto report = matching_engine::measurement::run_self_check(
      {}, {}, capabilities,
      {.samples = options.samples,
       .calibration_window_ns = static_cast<std::uint64_t>(options.calibration_ms) * 1'000'000U});

  std::cout << std::setprecision(12) << "{\"platform\":\"" << platform_name()
            << "\",\"compiler\":\"" << compiler_name() << "\",\"compiler_version\":\""
            << compiler_version() << "\",\"source\":\""
            << matching_engine::measurement::source_name(report.source)
            << "\",\"steady\":" << (capabilities.steady ? "true" : "false")
            << ",\"migration_detection\":" << (capabilities.migration_detection ? "true" : "false")
            << ",\"requested_samples\":" << report.requested_samples
            << ",\"valid_samples\":" << report.valid_samples
            << ",\"zero_deltas\":" << report.zero_deltas
            << ",\"backward_reads\":" << report.backward_reads
            << ",\"migration_discards\":" << report.migration_discards
            << ",\"min_overhead_ticks\":" << report.min_overhead_ticks
            << ",\"median_overhead_ticks\":" << report.median_overhead_ticks
            << ",\"p99_overhead_ticks\":" << report.p99_overhead_ticks
            << ",\"effective_granularity_ticks\":" << report.effective_granularity_ticks
            << ",\"ticks_per_ns\":" << report.ticks_per_ns
            << ",\"calibration_uncertainty\":" << report.calibration_uncertainty
            << ",\"calibrated\":" << (report.calibrated ? "true" : "false")
            << ",\"percentiles_publishable\":"
            << (report.percentiles_publishable ? "true" : "false") << ",\"reason\":\""
            << matching_engine::measurement::failure_reason_name(report.reason) << "\"}\n";

  std::cerr << "clock_probe: " << matching_engine::measurement::failure_reason_name(report.reason)
            << " (" << matching_engine::measurement::source_name(report.source)
            << ", effective granularity " << report.effective_granularity_ticks << " ticks)\n";
  return report.reason == matching_engine::measurement::FailureReason::passed ? 0 : 1;
}
