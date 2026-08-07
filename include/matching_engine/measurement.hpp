#pragma once

#include <cstdint>

namespace matching_engine::measurement {

enum class ClockSourceKind : std::uint8_t {
  x86_rdtscp,
  steady_clock_ns,
  unsupported,
};

struct ClockCapabilities {
  ClockSourceKind kind{ClockSourceKind::unsupported};
  bool steady{};
  bool migration_detection{};
  bool publication_capable{};
};

struct X86ClockFeatures {
  std::uint32_t maximum_extended_leaf{};
  bool rdtscp{};
  bool invariant_tsc{};
};

struct ClockSample {
  std::uint64_t ticks{};
  std::uint32_t cpu_aux{};
};

enum class ElapsedStatus : std::uint8_t {
  valid,
  backward,
  migrated,
};

struct ClockReader {
  void* context{};
  ClockSample (*read)(void*) noexcept {};
};

struct NanosecondReader {
  void* context{};
  std::uint64_t (*read)(void*) noexcept {};
};

enum class SelfCheckReason : std::uint8_t {
  clock_safe,
  unsupported_source,
  non_steady_clock,
  excessive_invalid_samples,
  excessive_zero_deltas,
  zero_observable_granularity,
  calibration_failure,
  calibration_instability,
};

enum class PublicationReason : std::uint8_t {
  qualified,
  clock_self_check_failed,
  source_regression_only,
  operation_not_evaluated,
  operation_below_resolution,
};

struct SelfCheckConfig {
  std::uint32_t samples{10'000};
  std::uint32_t calibration_windows{5};
  std::uint64_t calibration_window_ns{10'000'000};
  std::uint32_t maximum_invalid_percent{10};
  std::uint32_t maximum_zero_percent{90};
  double maximum_calibration_spread{0.05};
  std::uint32_t resolution_multiple{10};
  bool operation_evaluated{};
  std::uint64_t operation_median_ticks{};
};

struct SelfCheckReport {
  ClockSourceKind source{ClockSourceKind::unsupported};
  std::uint32_t requested_samples{};
  std::uint32_t valid_samples{};
  std::uint32_t zero_deltas{};
  std::uint32_t backward_reads{};
  std::uint32_t migration_discards{};
  std::uint64_t min_overhead_ticks{};
  std::uint64_t median_overhead_ticks{};
  std::uint64_t p99_overhead_ticks{};
  std::uint64_t effective_granularity_ticks{};
  double ticks_per_ns{};
  double calibration_uncertainty{};
  bool calibrated{};
  bool clock_safe{};
  bool source_publishable{};
  bool operation_evaluated{};
  bool operation_percentiles_publishable{};
  std::uint32_t zero_delta_threshold_percent{};
  SelfCheckReason self_check_reason{SelfCheckReason::unsupported_source};
  PublicationReason publication_reason{PublicationReason::clock_self_check_failed};
};

struct CalibrationBracket {
  ClockSample before{};
  std::uint64_t steady_nanoseconds{};
  ClockSample after{};
};

[[nodiscard]] ClockCapabilities select_clock_source(bool is_x86, bool steady_clock_available,
                                                    X86ClockFeatures features) noexcept;
[[nodiscard]] ClockCapabilities clock_capabilities() noexcept;
[[nodiscard]] ClockSample read_clock() noexcept;
[[nodiscard]] std::uint64_t read_steady_nanoseconds() noexcept;
[[nodiscard]] ElapsedStatus elapsed_ticks(ClockSample start, ClockSample end,
                                          ClockCapabilities capabilities,
                                          std::uint64_t& ticks) noexcept;
[[nodiscard]] SelfCheckReport run_self_check(ClockReader clock, NanosecondReader steady,
                                             ClockCapabilities capabilities,
                                             SelfCheckConfig config);
[[nodiscard]] bool calibration_window_ratio(CalibrationBracket start, CalibrationBracket end,
                                            ClockCapabilities capabilities,
                                            double& ticks_per_ns) noexcept;
[[nodiscard]] bool ticks_to_nanoseconds(std::uint64_t ticks, double ticks_per_ns,
                                        std::uint64_t& nanoseconds) noexcept;
[[nodiscard]] const char* source_name(ClockSourceKind source) noexcept;
[[nodiscard]] const char* self_check_reason_name(SelfCheckReason reason) noexcept;
[[nodiscard]] const char* publication_reason_name(PublicationReason reason) noexcept;

} // namespace matching_engine::measurement
