#include "matching_engine/measurement.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

namespace {

using matching_engine::measurement::ClockCapabilities;
using matching_engine::measurement::ClockReader;
using matching_engine::measurement::ClockSample;
using matching_engine::measurement::ClockSourceKind;
using matching_engine::measurement::ElapsedStatus;
using matching_engine::measurement::NanosecondReader;
using matching_engine::measurement::PublicationReason;
using matching_engine::measurement::SelfCheckConfig;
using matching_engine::measurement::SelfCheckReason;

template <typename T, std::size_t Size> struct Sequence {
  std::array<T, Size> values{};
  std::size_t position{};

  [[nodiscard]] static T next(void* context) noexcept {
    auto& sequence = *static_cast<Sequence*>(context);
    if (sequence.position >= Size) {
      return sequence.values.back();
    }
    return sequence.values[sequence.position++];
  }
};

constexpr ClockCapabilities kRdtscp{
    .kind = ClockSourceKind::x86_rdtscp,
    .steady = true,
    .migration_detection = true,
    .publication_capable = true,
};
constexpr ClockCapabilities kFallback{
    .kind = ClockSourceKind::steady_clock_ns,
    .steady = true,
    .migration_detection = false,
    .publication_capable = false,
};

TEST(MeasurementElapsed, RejectsBackwardAndMigratedSamples) {
  std::uint64_t ticks{};
  EXPECT_EQ(matching_engine::measurement::elapsed_ticks({20, 1}, {19, 1}, kRdtscp, ticks),
            ElapsedStatus::backward);
  EXPECT_EQ(matching_engine::measurement::elapsed_ticks({20, 1}, {21, 2}, kRdtscp, ticks),
            ElapsedStatus::migrated);
  EXPECT_EQ(matching_engine::measurement::elapsed_ticks({20, 3}, {25, 3}, kRdtscp, ticks),
            ElapsedStatus::valid);
  EXPECT_EQ(ticks, 5U);
}

TEST(MeasurementSelfCheck, ReportsDeterministicDistributionAndGranularity) {
  Sequence<ClockSample, 10> clock{
      {{{10, 1}, {10, 1}, {20, 1}, {22, 1}, {30, 1}, {34, 1}, {40, 1}, {48, 1}, {50, 1}, {66, 1}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 5, .calibration_windows = 0});

  EXPECT_EQ(report.zero_deltas, 1U);
  EXPECT_EQ(report.backward_reads, 0U);
  EXPECT_EQ(report.migration_discards, 0U);
  EXPECT_EQ(report.valid_samples, 5U);
  EXPECT_EQ(report.min_overhead_ticks, 0U);
  EXPECT_EQ(report.median_overhead_ticks, 4U);
  EXPECT_EQ(report.p99_overhead_ticks, 16U);
  EXPECT_EQ(report.effective_granularity_ticks, 2U);
  EXPECT_TRUE(report.clock_safe);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::clock_safe);
}

TEST(MeasurementSelfCheck, CountsInvalidSamplesAndRefusesPercentiles) {
  Sequence<ClockSample, 8> clock{
      {{{10, 1}, {9, 1}, {20, 1}, {21, 2}, {30, 1}, {30, 1}, {40, 1}, {42, 1}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kRdtscp,
      SelfCheckConfig{.samples = 4, .calibration_windows = 0});

  EXPECT_EQ(report.backward_reads, 1U);
  EXPECT_EQ(report.migration_discards, 1U);
  EXPECT_EQ(report.zero_deltas, 1U);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::excessive_invalid_samples);
  EXPECT_FALSE(report.clock_safe);
}

TEST(MeasurementSelfCheck, RejectsZeroObservableGranularity) {
  Sequence<ClockSample, 6> clock{{{{10, 1}, {10, 1}, {10, 1}, {10, 1}, {10, 1}, {10, 1}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 3, .calibration_windows = 0});

  EXPECT_EQ(report.self_check_reason, SelfCheckReason::zero_observable_granularity);
  EXPECT_FALSE(report.clock_safe);
}

TEST(MeasurementSelfCheck, RejectsUnstableCalibration) {
  Sequence<ClockSample, 14> clock{{{{0, 1},
                                    {2, 1},
                                    {100, 1},
                                    {102, 1},
                                    {200, 1},
                                    {202, 1},
                                    {300, 1},
                                    {302, 1},
                                    {500, 1},
                                    {502, 1},
                                    {600, 1},
                                    {602, 1},
                                    {900, 1},
                                    {902, 1}}}};
  Sequence<std::uint64_t, 6> steady{{{0, 100, 200, 300, 400, 500}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 1, .calibration_windows = 3});

  EXPECT_EQ(report.self_check_reason, SelfCheckReason::calibration_instability);
  EXPECT_FALSE(report.calibrated);
  EXPECT_FALSE(report.clock_safe);
}

TEST(MeasurementSelfCheck, FallbackOperationRemainsRegressionOnly) {
  Sequence<ClockSample, 6> clock{{{{10, 1}, {12, 1}, {20, 1}, {22, 1}, {30, 1}, {32, 1}}}};
  const auto report =
      matching_engine::measurement::run_self_check({&clock, &decltype(clock)::next}, {}, kFallback,
                                                   SelfCheckConfig{.samples = 3,
                                                                   .calibration_windows = 0,
                                                                   .operation_evaluated = true,
                                                                   .operation_median_ticks = 19});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.publication_reason, PublicationReason::source_regression_only);
}

TEST(MeasurementSelfCheck, SteadyFallbackHasExactUnitRatioWithoutCalibrationClaim) {
  Sequence<ClockSample, 4> clock{{{{10, 0}, {12, 0}, {20, 0}, {24, 0}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 2, .calibration_windows = 5});

  EXPECT_DOUBLE_EQ(report.ticks_per_ns, 1.0);
  EXPECT_DOUBLE_EQ(report.calibration_uncertainty, 0.0);
  EXPECT_FALSE(report.calibrated);
  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.operation_percentiles_publishable);
}

TEST(MeasurementSelfCheck, RejectsX86SourceWithoutCalibrationWindows) {
  Sequence<ClockSample, 4> clock{{{{10, 1}, {12, 1}, {20, 1}, {24, 1}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kRdtscp,
      SelfCheckConfig{.samples = 2, .calibration_windows = 0});

  EXPECT_EQ(report.self_check_reason, SelfCheckReason::calibration_failure);
  EXPECT_FALSE(report.clock_safe);
}

TEST(MeasurementSelfCheck, ReportsMedianStableCalibration) {
  Sequence<ClockSample, 16> clock{{{{0, 1},
                                    {2, 1},
                                    {10, 1},
                                    {12, 1},
                                    {100, 1},
                                    {104, 1},
                                    {300, 1},
                                    {304, 1},
                                    {400, 1},
                                    {404, 1},
                                    {602, 1},
                                    {606, 1},
                                    {700, 1},
                                    {704, 1},
                                    {898, 1},
                                    {902, 1}}}};
  Sequence<std::uint64_t, 6> steady{{{0, 100, 200, 300, 400, 500}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 2, .calibration_windows = 3});

  EXPECT_EQ(report.self_check_reason, SelfCheckReason::clock_safe);
  EXPECT_DOUBLE_EQ(report.ticks_per_ns, 2.0);
  EXPECT_NEAR(report.calibration_uncertainty, 0.02, 1e-12);
  EXPECT_TRUE(report.calibrated);
  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.operation_percentiles_publishable);
}

TEST(MeasurementConversion, RejectsInvalidAndOutOfRangeNarrowing) {
  std::uint64_t nanoseconds{};
  EXPECT_FALSE(matching_engine::measurement::ticks_to_nanoseconds(1, 0.0, nanoseconds));
  EXPECT_FALSE(matching_engine::measurement::ticks_to_nanoseconds(
      1, std::numeric_limits<double>::infinity(), nanoseconds));
  EXPECT_FALSE(matching_engine::measurement::ticks_to_nanoseconds(
      std::numeric_limits<std::uint64_t>::max(), 0.5, nanoseconds));
  EXPECT_TRUE(matching_engine::measurement::ticks_to_nanoseconds(15, 2.0, nanoseconds));
  EXPECT_EQ(nanoseconds, 7U);
}

TEST(MeasurementClock, RealClockProducesValidSamples) {
  const auto capabilities = matching_engine::measurement::clock_capabilities();
  const auto first = matching_engine::measurement::read_clock();
  const auto second = matching_engine::measurement::read_clock();
  std::uint64_t ticks{};
  const auto status =
      matching_engine::measurement::elapsed_ticks(first, second, capabilities, ticks);
  EXPECT_TRUE(status == ElapsedStatus::valid || status == ElapsedStatus::migrated);
}

#if defined(__x86_64__) || defined(_M_X64)
TEST(MeasurementClock, X86ClockExposesAuxAwareElapsedBehavior) {
  const auto capabilities = matching_engine::measurement::clock_capabilities();
  EXPECT_TRUE(capabilities.kind == ClockSourceKind::x86_rdtscp ||
              capabilities.kind == ClockSourceKind::steady_clock_ns);
  std::uint64_t ticks{};
  EXPECT_EQ(matching_engine::measurement::elapsed_ticks({1, 7}, {2, 8}, kRdtscp, ticks),
            ElapsedStatus::migrated);
}
#endif

static_assert(noexcept(matching_engine::measurement::read_clock()));

} // namespace
