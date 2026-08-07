#include "matching_engine/measurement.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

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

[[nodiscard]] FailureReason validate_source(ClockCapabilities capabilities) noexcept {
  if (capabilities.kind == ClockSourceKind::unsupported) {
    return FailureReason::unsupported_source;
  }
  return capabilities.steady ? FailureReason::passed : FailureReason::non_steady_clock;
}

} // namespace

ClockCapabilities clock_capabilities() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  return {.kind = ClockSourceKind::x86_rdtscp, .steady = true, .migration_detection = true};
#else
  return {.kind = ClockSourceKind::steady_clock_ns,
          .steady = std::chrono::steady_clock::is_steady,
          .migration_detection = false};
#endif
}

ClockSample read_clock() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  std::uint32_t low{};
  std::uint32_t high{};
  std::uint32_t auxiliary{};
  __asm__ volatile("lfence\n\trdtscp\n\tlfence"
                   : "=a"(low), "=d"(high), "=c"(auxiliary)
                   :
                   : "memory");
  return {.ticks = (static_cast<std::uint64_t>(high) << 32U) | low, .cpu_aux = auxiliary};
#elif defined(_M_X64)
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
      .reason = validate_source(capabilities),
  };
  if (report.reason != FailureReason::passed || config.samples == 0) {
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
  if (static_cast<std::uint64_t>(invalid) * 100U >
      static_cast<std::uint64_t>(config.samples) * config.maximum_invalid_percent) {
    report.reason = FailureReason::excessive_invalid_samples;
    return report;
  }

  std::ranges::sort(deltas);
  if (!deltas.empty()) {
    report.min_overhead_ticks = deltas.front();
    report.median_overhead_ticks = deltas[deltas.size() / 2U];
    const auto p99_index = ((deltas.size() * 99U) + 99U) / 100U - 1U;
    report.p99_overhead_ticks = deltas[p99_index];
    const auto nonzero =
        std::ranges::find_if(deltas, [](std::uint64_t value) { return value != 0; });
    if (nonzero != deltas.end()) {
      report.effective_granularity_ticks = *nonzero;
    }
  }
  if (report.effective_granularity_ticks == 0) {
    report.reason = FailureReason::zero_observable_granularity;
    return report;
  }

  if (capabilities.kind == ClockSourceKind::steady_clock_ns) {
    report.ticks_per_ns = 1.0;
  } else if (config.calibration_windows == 0) {
    report.reason = FailureReason::calibration_instability;
    return report;
  } else {
    if (steady.read == nullptr) {
      steady.read = &default_steady_reader;
    }
    std::vector<double> ratios;
    ratios.reserve(config.calibration_windows);
    for (std::uint32_t window = 0; window < config.calibration_windows; ++window) {
      const auto steady_start = steady.read(steady.context);
      const auto clock_start = clock.read(clock.context);
      auto steady_end = steady.read(steady.context);
      while (steady_end >= steady_start &&
             steady_end - steady_start < config.calibration_window_ns) {
        steady_end = steady.read(steady.context);
      }
      const auto clock_end = clock.read(clock.context);
      std::uint64_t clock_delta{};
      if (steady_end <= steady_start ||
          elapsed_ticks(clock_start, clock_end, capabilities, clock_delta) !=
              ElapsedStatus::valid ||
          clock_delta == 0) {
        report.reason = FailureReason::calibration_instability;
        return report;
      }
      ratios.push_back(static_cast<double>(clock_delta) /
                       static_cast<double>(steady_end - steady_start));
    }
    std::ranges::sort(ratios);
    report.ticks_per_ns = ratios[ratios.size() / 2U];
    report.calibration_uncertainty = (ratios.back() - ratios.front()) / report.ticks_per_ns;
    report.calibrated = true;
    if (!std::isfinite(report.ticks_per_ns) || report.ticks_per_ns <= 0.0 ||
        !std::isfinite(report.calibration_uncertainty) ||
        report.calibration_uncertainty > config.maximum_calibration_spread) {
      report.reason = FailureReason::calibration_instability;
      return report;
    }
  }

  if (config.operation_median_ticks != 0 && config.resolution_multiple != 0) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (report.effective_granularity_ticks > maximum / config.resolution_multiple ||
        config.operation_median_ticks <
            report.effective_granularity_ticks * config.resolution_multiple) {
      report.reason = FailureReason::operation_below_resolution;
      return report;
    }
  }

  report.percentiles_publishable = true;
  return report;
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

const char* failure_reason_name(FailureReason reason) noexcept {
  switch (reason) {
  case FailureReason::passed:
    return "passed";
  case FailureReason::unsupported_source:
    return "unsupported_source";
  case FailureReason::non_steady_clock:
    return "non_steady_clock";
  case FailureReason::excessive_invalid_samples:
    return "excessive_invalid_samples";
  case FailureReason::zero_observable_granularity:
    return "zero_observable_granularity";
  case FailureReason::calibration_instability:
    return "calibration_instability";
  case FailureReason::operation_below_resolution:
    return "operation_below_resolution";
  }
  return "unsupported_source";
}

} // namespace matching_engine::measurement
