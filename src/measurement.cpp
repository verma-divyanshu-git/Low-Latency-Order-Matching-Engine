#include "matching_engine/measurement.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <thread>
#include <vector>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#endif
#if defined(_M_X64)
#include <intrin.h>
#endif

namespace matching_engine::measurement {
namespace {

[[nodiscard]] ClockSample default_clock_reader(void*) noexcept {
  return read_clock();
}

[[nodiscard]] std::uint64_t default_steady_reader(void*) noexcept {
  return read_steady_nanoseconds();
}

[[nodiscard]] SelfCheckReason validate_source(ClockCapabilities capabilities) noexcept {
  if (capabilities.kind == ClockSourceKind::unsupported) {
    return SelfCheckReason::unsupported_source;
  }
  return capabilities.steady ? SelfCheckReason::clock_safe : SelfCheckReason::non_steady_clock;
}

[[maybe_unused, nodiscard]] X86ClockFeatures detect_x86_clock_features() noexcept {
  X86ClockFeatures features{};
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  features.maximum_extended_leaf = __get_cpuid_max(0x80000000U, nullptr);
  unsigned int eax{};
  unsigned int ebx{};
  unsigned int ecx{};
  unsigned int edx{};
  if (features.maximum_extended_leaf >= 0x80000001U &&
      __get_cpuid(0x80000001U, &eax, &ebx, &ecx, &edx) != 0) {
    features.rdtscp = (edx & (1U << 27U)) != 0;
  }
  if (features.maximum_extended_leaf >= 0x80000007U &&
      __get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx) != 0) {
    features.invariant_tsc = (edx & (1U << 8U)) != 0;
  }
#elif defined(_M_X64)
  int registers[4]{};
  __cpuid(registers, static_cast<int>(0x80000000U));
  features.maximum_extended_leaf = static_cast<std::uint32_t>(registers[0]);
  if (features.maximum_extended_leaf >= 0x80000001U) {
    __cpuid(registers, static_cast<int>(0x80000001U));
    features.rdtscp = (static_cast<std::uint32_t>(registers[3]) & (1U << 27U)) != 0;
  }
  if (features.maximum_extended_leaf >= 0x80000007U) {
    __cpuid(registers, static_cast<int>(0x80000007U));
    features.invariant_tsc = (static_cast<std::uint32_t>(registers[3]) & (1U << 8U)) != 0;
  }
#endif
  return features;
}

[[nodiscard]] std::size_t nearest_rank_index(std::size_t count, std::size_t percentile) noexcept {
  return ((count * percentile) + 99U) / 100U - 1U;
}

[[nodiscard]] CalibrationBracket read_calibration_bracket(ClockReader clock,
                                                          NanosecondReader steady) noexcept {
  return {
      .before = clock.read(clock.context),
      .steady_nanoseconds = steady.read(steady.context),
      .after = clock.read(clock.context),
  };
}

} // namespace

ClockCapabilities select_clock_source(bool is_x86, bool steady_clock_available,
                                      X86ClockFeatures features) noexcept {
  if (is_x86 && features.maximum_extended_leaf >= 0x80000007U && features.rdtscp &&
      features.invariant_tsc) {
    return {.kind = ClockSourceKind::x86_rdtscp,
            .steady = true,
            .migration_detection = true,
            .publication_capable = true};
  }
  if (steady_clock_available) {
    return {.kind = ClockSourceKind::steady_clock_ns,
            .steady = true,
            .migration_detection = false,
            .publication_capable = false};
  }
  return {};
}

ClockCapabilities clock_capabilities() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  return select_clock_source(true, std::chrono::steady_clock::is_steady,
                             detect_x86_clock_features());
#else
  return select_clock_source(false, std::chrono::steady_clock::is_steady, {});
#endif
}

ClockSample read_clock() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const auto capabilities = clock_capabilities();
  if (capabilities.kind != ClockSourceKind::x86_rdtscp) {
    return {.ticks = read_steady_nanoseconds(), .cpu_aux = 0};
  }
  std::uint32_t low{};
  std::uint32_t high{};
  std::uint32_t auxiliary{};
  __asm__ volatile("lfence\n\trdtscp\n\tlfence"
                   : "=a"(low), "=d"(high), "=c"(auxiliary)
                   :
                   : "memory");
  return {.ticks = (static_cast<std::uint64_t>(high) << 32U) | low, .cpu_aux = auxiliary};
#elif defined(_M_X64)
  static const auto capabilities = clock_capabilities();
  if (capabilities.kind != ClockSourceKind::x86_rdtscp) {
    return {.ticks = read_steady_nanoseconds(), .cpu_aux = 0};
  }
  unsigned int auxiliary{};
  _mm_lfence();
  const auto ticks = __rdtscp(&auxiliary);
  _mm_lfence();
  return {.ticks = ticks, .cpu_aux = auxiliary};
#else
  return {.ticks = read_steady_nanoseconds(), .cpu_aux = 0};
#endif
}

std::uint64_t read_steady_nanoseconds() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 0;
}

ElapsedStatus elapsed_ticks(ClockSample start, ClockSample end, ClockCapabilities capabilities,
                            std::uint64_t& ticks) noexcept {
  if (capabilities.migration_detection && start.cpu_aux != end.cpu_aux) {
    return ElapsedStatus::migrated;
  }
  if (end.ticks < start.ticks) {
    return ElapsedStatus::backward;
  }
  ticks = end.ticks - start.ticks;
  return ElapsedStatus::valid;
}

SelfCheckReport run_self_check(ClockReader clock, NanosecondReader steady,
                               ClockCapabilities capabilities, SelfCheckConfig config) {
  SelfCheckReport report{
      .source = capabilities.kind,
      .requested_samples = config.samples,
      .operation_evaluated = config.operation_evaluated,
      .zero_delta_threshold_percent = config.maximum_zero_percent,
      .self_check_reason = validate_source(capabilities),
  };
  if (report.self_check_reason != SelfCheckReason::clock_safe || config.samples == 0) {
    return report;
  }
  if (clock.read == nullptr) {
    clock.read = &default_clock_reader;
  }

  std::vector<std::uint64_t> deltas;
  deltas.reserve(config.samples);
  for (std::uint32_t sample = 0; sample < config.samples; ++sample) {
    const auto start = clock.read(clock.context);
    const auto end = clock.read(clock.context);
    std::uint64_t delta{};
    switch (elapsed_ticks(start, end, capabilities, delta)) {
    case ElapsedStatus::valid:
      ++report.valid_samples;
      report.zero_deltas += delta == 0 ? 1U : 0U;
      deltas.push_back(delta);
      break;
    case ElapsedStatus::backward:
      ++report.backward_reads;
      break;
    case ElapsedStatus::migrated:
      ++report.migration_discards;
      break;
    }
  }

  const auto invalid = report.backward_reads + report.migration_discards;
  std::ranges::sort(deltas);
  if (!deltas.empty()) {
    report.min_overhead_ticks = deltas.front();
    report.median_overhead_ticks = deltas[nearest_rank_index(deltas.size(), 50U)];
    report.p99_overhead_ticks = deltas[nearest_rank_index(deltas.size(), 99U)];
    const auto nonzero =
        std::ranges::find_if(deltas, [](std::uint64_t value) { return value != 0; });
    if (nonzero != deltas.end()) {
      report.effective_granularity_ticks = *nonzero;
    }
  }
  if (static_cast<std::uint64_t>(invalid) * 100U >
      static_cast<std::uint64_t>(config.samples) * config.maximum_invalid_percent) {
    report.self_check_reason = SelfCheckReason::excessive_invalid_samples;
    return report;
  }
  if (report.effective_granularity_ticks == 0) {
    report.self_check_reason = SelfCheckReason::zero_observable_granularity;
    return report;
  }
  if (static_cast<std::uint64_t>(report.zero_deltas) * 100U >
      static_cast<std::uint64_t>(report.valid_samples) * config.maximum_zero_percent) {
    report.self_check_reason = SelfCheckReason::excessive_zero_deltas;
    return report;
  }

  if (capabilities.kind == ClockSourceKind::steady_clock_ns) {
    report.ticks_per_ns = 1.0;
    report.clock_safe = true;
    report.publication_reason = PublicationReason::source_regression_only;
    return report;
  } else if (config.calibration_windows == 0) {
    report.self_check_reason = SelfCheckReason::calibration_failure;
    return report;
  } else {
    const bool use_real_window = steady.read == nullptr;
    if (steady.read == nullptr) {
      steady.read = &default_steady_reader;
    }
    std::vector<double> ratios;
    ratios.reserve(config.calibration_windows);
    for (std::uint32_t window = 0; window < config.calibration_windows; ++window) {
      const auto start = read_calibration_bracket(clock, steady);
      if (use_real_window) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(config.calibration_window_ns));
      }
      const auto end = read_calibration_bracket(clock, steady);
      double ratio{};
      if (!calibration_window_ratio(start, end, capabilities, ratio)) {
        report.self_check_reason = SelfCheckReason::calibration_failure;
        return report;
      }
      ratios.push_back(ratio);
    }
    std::ranges::sort(ratios);
    report.ticks_per_ns = ratios[nearest_rank_index(ratios.size(), 50U)];
    report.calibration_uncertainty = (ratios.back() - ratios.front()) / report.ticks_per_ns;
    if (!std::isfinite(report.ticks_per_ns) || report.ticks_per_ns <= 0.0 ||
        !std::isfinite(report.calibration_uncertainty) ||
        report.calibration_uncertainty > config.maximum_calibration_spread) {
      report.self_check_reason = SelfCheckReason::calibration_instability;
      return report;
    }
    report.calibrated = true;
    report.source_publishable = capabilities.publication_capable;
  }

  report.clock_safe = true;
  if (!report.source_publishable) {
    report.publication_reason = PublicationReason::source_regression_only;
    return report;
  }
  if (!config.operation_evaluated) {
    report.publication_reason = PublicationReason::operation_not_evaluated;
    return report;
  }
  if (config.resolution_multiple != 0) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (report.effective_granularity_ticks > maximum / config.resolution_multiple ||
        config.operation_median_ticks <
            report.effective_granularity_ticks * config.resolution_multiple) {
      report.publication_reason = PublicationReason::operation_below_resolution;
      return report;
    }
  }

  report.operation_percentiles_publishable = true;
  report.publication_reason = PublicationReason::qualified;
  return report;
}

bool calibration_window_ratio(CalibrationBracket start, CalibrationBracket end,
                              ClockCapabilities capabilities, double& ticks_per_ns) noexcept {
  std::uint64_t start_width{};
  std::uint64_t end_width{};
  if (end.steady_nanoseconds <= start.steady_nanoseconds ||
      elapsed_ticks(start.before, start.after, capabilities, start_width) != ElapsedStatus::valid ||
      elapsed_ticks(end.before, end.after, capabilities, end_width) != ElapsedStatus::valid) {
    return false;
  }
  const ClockSample start_midpoint{
      .ticks = start.before.ticks + (start_width / 2U),
      .cpu_aux = start.before.cpu_aux,
  };
  const ClockSample end_midpoint{
      .ticks = end.before.ticks + (end_width / 2U),
      .cpu_aux = end.before.cpu_aux,
  };
  std::uint64_t clock_delta{};
  if (elapsed_ticks(start_midpoint, end_midpoint, capabilities, clock_delta) !=
          ElapsedStatus::valid ||
      clock_delta == 0) {
    return false;
  }
  ticks_per_ns = static_cast<double>(clock_delta) /
                 static_cast<double>(end.steady_nanoseconds - start.steady_nanoseconds);
  return std::isfinite(ticks_per_ns) && ticks_per_ns > 0.0;
}

bool ticks_to_nanoseconds(std::uint64_t ticks, double ticks_per_ns,
                          std::uint64_t& nanoseconds) noexcept {
  if (!std::isfinite(ticks_per_ns) || ticks_per_ns <= 0.0) {
    return false;
  }
  const auto converted = static_cast<long double>(ticks) / ticks_per_ns;
  if (!std::isfinite(converted) || converted < 0.0L ||
      converted > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  nanoseconds = static_cast<std::uint64_t>(converted);
  return true;
}

const char* source_name(ClockSourceKind source) noexcept {
  switch (source) {
  case ClockSourceKind::x86_rdtscp:
    return "x86_rdtscp";
  case ClockSourceKind::steady_clock_ns:
    return "steady_clock_ns";
  case ClockSourceKind::unsupported:
    return "unsupported";
  }
  return "unsupported";
}

const char* self_check_reason_name(SelfCheckReason reason) noexcept {
  switch (reason) {
  case SelfCheckReason::clock_safe:
    return "clock_safe";
  case SelfCheckReason::unsupported_source:
    return "unsupported_source";
  case SelfCheckReason::non_steady_clock:
    return "non_steady_clock";
  case SelfCheckReason::excessive_invalid_samples:
    return "excessive_invalid_samples";
  case SelfCheckReason::excessive_zero_deltas:
    return "excessive_zero_deltas";
  case SelfCheckReason::zero_observable_granularity:
    return "zero_observable_granularity";
  case SelfCheckReason::calibration_failure:
    return "calibration_failure";
  case SelfCheckReason::calibration_instability:
    return "calibration_instability";
  }
  return "unsupported_source";
}

const char* publication_reason_name(PublicationReason reason) noexcept {
  switch (reason) {
  case PublicationReason::qualified:
    return "qualified";
  case PublicationReason::clock_self_check_failed:
    return "clock_self_check_failed";
  case PublicationReason::source_regression_only:
    return "source_regression_only";
  case PublicationReason::operation_not_evaluated:
    return "operation_not_evaluated";
  case PublicationReason::operation_below_resolution:
    return "operation_below_resolution";
  }
  return "clock_self_check_failed";
}

} // namespace matching_engine::measurement
