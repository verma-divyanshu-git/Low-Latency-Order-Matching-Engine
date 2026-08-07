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
  const auto compiler = matching_engine::measurement::compiler_metadata();
  const auto report = matching_engine::measurement::run_self_check(
      {}, {}, capabilities,
      {.samples = options.samples,
       .calibration_window_ns = static_cast<std::uint64_t>(options.calibration_ms) * 1'000'000U});

  std::cout << std::setprecision(12) << "{\"platform\":\"" << platform_name()
            << "\",\"compiler_family\":\""
            << matching_engine::measurement::compiler_family_name(compiler.family)
            << "\",\"compiler_major\":" << compiler.major
            << ",\"compiler_minor\":" << compiler.minor << ",\"compiler_patch\":" << compiler.patch
            << ",\"source\":\"" << matching_engine::measurement::source_name(report.source)
            << "\",\"steady\":" << (capabilities.steady ? "true" : "false")
            << ",\"migration_detection\":" << (capabilities.migration_detection ? "true" : "false")
            << ",\"source_publication_capable\":"
            << (capabilities.publication_capable ? "true" : "false")
            << ",\"requested_samples\":" << report.requested_samples
            << ",\"valid_samples\":" << report.valid_samples
            << ",\"zero_deltas\":" << report.zero_deltas
            << ",\"zero_delta_threshold_percent\":" << report.zero_delta_threshold_percent
            << ",\"backward_reads\":" << report.backward_reads
            << ",\"migration_discards\":" << report.migration_discards
            << ",\"min_overhead_ticks\":" << report.min_overhead_ticks
            << ",\"median_overhead_ticks\":" << report.median_overhead_ticks
            << ",\"p99_overhead_ticks\":" << report.p99_overhead_ticks
            << ",\"effective_granularity_ticks\":" << report.effective_granularity_ticks
            << ",\"ticks_per_ns\":" << report.ticks_per_ns
            << ",\"calibration_uncertainty\":" << report.calibration_uncertainty
            << ",\"calibrated\":" << (report.calibrated ? "true" : "false")
            << ",\"clock_safe\":" << (report.clock_safe ? "true" : "false")
            << ",\"source_publishable\":" << (report.source_publishable ? "true" : "false")
            << ",\"operation_evaluated\":" << (report.operation_evaluated ? "true" : "false")
            << ",\"operation_percentiles_publishable\":"
            << (report.operation_percentiles_publishable ? "true" : "false")
            << ",\"self_check_reason\":\""
            << matching_engine::measurement::self_check_reason_name(report.self_check_reason)
            << "\",\"publication_reason\":\""
            << matching_engine::measurement::publication_reason_name(report.publication_reason)
            << "\"}\n";

  std::cerr << "clock_probe: "
            << matching_engine::measurement::self_check_reason_name(report.self_check_reason)
            << ", publication "
            << matching_engine::measurement::publication_reason_name(report.publication_reason)
            << " (" << matching_engine::measurement::source_name(report.source)
            << ", effective granularity " << report.effective_granularity_ticks << " ticks)\n";
  return report.clock_safe ? 0 : 1;
}
