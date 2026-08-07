#include "matching_engine/clock_probe_cli.hpp"
#include "matching_engine/measurement.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string_view>

namespace {

using matching_engine::measurement::CalibrationBracket;
using matching_engine::measurement::ClockCapabilities;
using matching_engine::measurement::ClockSample;
using matching_engine::measurement::ClockSourceKind;
using matching_engine::measurement::PublicationReason;
using matching_engine::measurement::SelfCheckConfig;
using matching_engine::measurement::SelfCheckReason;
using matching_engine::measurement::X86ClockFeatures;

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

[[nodiscard]] Sequence<ClockSample, 16> stable_calibration_clock() {
  return {{{{0, 1},
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
}

[[nodiscard]] Sequence<std::uint64_t, 6> stable_calibration_steady() {
  return {{{0, 100, 200, 300, 400, 500}}};
}

TEST(MeasurementSourceSelection, RequiresEveryRdtscpCapability) {
  constexpr X86ClockFeatures supported{
      .maximum_extended_leaf = 0x80000007U,
      .rdtscp = true,
      .invariant_tsc = true,
  };
  const auto qualified = matching_engine::measurement::select_clock_source(true, true, supported);
  EXPECT_EQ(qualified.kind, ClockSourceKind::x86_rdtscp);
  EXPECT_TRUE(qualified.publication_capable);

  auto missing_leaf = supported;
  missing_leaf.maximum_extended_leaf = 0x80000000U;
  EXPECT_EQ(matching_engine::measurement::select_clock_source(true, true, missing_leaf).kind,
            ClockSourceKind::steady_clock_ns);

  auto missing_invariant_leaf = supported;
  missing_invariant_leaf.maximum_extended_leaf = 0x80000001U;
  EXPECT_EQ(
      matching_engine::measurement::select_clock_source(true, true, missing_invariant_leaf).kind,
      ClockSourceKind::steady_clock_ns);

  auto missing_rdtscp = supported;
  missing_rdtscp.rdtscp = false;
  EXPECT_EQ(matching_engine::measurement::select_clock_source(true, true, missing_rdtscp).kind,
            ClockSourceKind::steady_clock_ns);

  auto missing_invariant_tsc = supported;
  missing_invariant_tsc.invariant_tsc = false;
  EXPECT_EQ(
      matching_engine::measurement::select_clock_source(true, true, missing_invariant_tsc).kind,
      ClockSourceKind::steady_clock_ns);
}

TEST(MeasurementSourceSelection, UsesFallbackForNonX86AndUnsupportedWithoutSteadyClock) {
  constexpr X86ClockFeatures features{};
  EXPECT_EQ(matching_engine::measurement::select_clock_source(false, true, features).kind,
            ClockSourceKind::steady_clock_ns);
  EXPECT_EQ(matching_engine::measurement::select_clock_source(true, false, features).kind,
            ClockSourceKind::unsupported);
}

TEST(MeasurementSelfCheckReview, SteadyFallbackIsSafeButNeverPublishable) {
  Sequence<ClockSample, 4> clock{{{{10, 0}, {12, 0}, {20, 0}, {24, 0}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 2, .calibration_windows = 0});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.source_publishable);
  EXPECT_FALSE(report.operation_evaluated);
  EXPECT_FALSE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::clock_safe);
  EXPECT_EQ(report.publication_reason, PublicationReason::source_regression_only);
}

TEST(MeasurementSelfCheckReview, QualifiedSourceWithoutOperationIsNotPublishable) {
  auto clock = stable_calibration_clock();
  auto steady = stable_calibration_steady();
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 2, .calibration_windows = 3});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_TRUE(report.source_publishable);
  EXPECT_FALSE(report.operation_evaluated);
  EXPECT_FALSE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.publication_reason, PublicationReason::operation_not_evaluated);
}

TEST(MeasurementSelfCheckReview, QualifiedResolvedOperationCanPublish) {
  auto clock = stable_calibration_clock();
  auto steady = stable_calibration_steady();
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 2,
                      .calibration_windows = 3,
                      .operation_evaluated = true,
                      .operation_median_ticks = 20});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_TRUE(report.source_publishable);
  EXPECT_TRUE(report.operation_evaluated);
  EXPECT_TRUE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.publication_reason, PublicationReason::qualified);
}

TEST(MeasurementSelfCheckReview, QualifiedUnresolvedOperationCannotPublish) {
  auto clock = stable_calibration_clock();
  auto steady = stable_calibration_steady();
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 2,
                      .calibration_windows = 3,
                      .operation_evaluated = true,
                      .operation_median_ticks = 19});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.publication_reason, PublicationReason::operation_below_resolution);
}

TEST(MeasurementSelfCheckReview, ResolutionThresholdOverflowCannotPublish) {
  Sequence<ClockSample, 14> clock{{{{0, 1},
                                    {std::numeric_limits<std::uint64_t>::max(), 1},
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
  auto steady = stable_calibration_steady();
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 1,
                      .calibration_windows = 3,
                      .resolution_multiple = 10,
                      .operation_evaluated = true,
                      .operation_median_ticks = std::numeric_limits<std::uint64_t>::max()});

  EXPECT_TRUE(report.clock_safe);
  EXPECT_FALSE(report.operation_percentiles_publishable);
  EXPECT_EQ(report.publication_reason, PublicationReason::operation_below_resolution);
}

TEST(MeasurementSelfCheckReview, ZeroHeavyDistributionIsReportedAndRejected) {
  Sequence<ClockSample, 10> clock{
      {{{10, 0}, {10, 0}, {20, 0}, {20, 0}, {30, 0}, {30, 0}, {40, 0}, {40, 0}, {50, 0}, {52, 0}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 5, .calibration_windows = 0, .maximum_zero_percent = 50});

  EXPECT_EQ(report.zero_deltas, 4U);
  EXPECT_EQ(report.zero_delta_threshold_percent, 50U);
  EXPECT_EQ(report.median_overhead_ticks, 0U);
  EXPECT_EQ(report.p99_overhead_ticks, 2U);
  EXPECT_EQ(report.effective_granularity_ticks, 2U);
  EXPECT_FALSE(report.clock_safe);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::excessive_zero_deltas);
}

TEST(MeasurementSelfCheckReview, UsesNearestRankForEvenSamplePercentiles) {
  Sequence<ClockSample, 8> clock{
      {{{0, 0}, {1, 0}, {10, 0}, {12, 0}, {20, 0}, {23, 0}, {30, 0}, {34, 0}}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {}, kFallback,
      SelfCheckConfig{.samples = 4, .calibration_windows = 0});

  EXPECT_EQ(report.median_overhead_ticks, 2U);
  EXPECT_EQ(report.p99_overhead_ticks, 4U);
}

TEST(MeasurementCalibration, UsesBracketMidpoints) {
  constexpr CalibrationBracket start{
      .before = {100, 1}, .steady_nanoseconds = 1'000, .after = {104, 1}};
  constexpr CalibrationBracket end{
      .before = {300, 1}, .steady_nanoseconds = 1'100, .after = {304, 1}};
  double ratio{};

  EXPECT_TRUE(matching_engine::measurement::calibration_window_ratio(start, end, kRdtscp, ratio));
  EXPECT_DOUBLE_EQ(ratio, 2.0);
}

TEST(MeasurementCalibration, FrozenInjectedSteadyReaderFailsWithoutPolling) {
  Sequence<ClockSample, 6> clock{{{{0, 1}, {2, 1}, {100, 1}, {104, 1}, {300, 1}, {304, 1}}}};
  Sequence<std::uint64_t, 2> steady{{{100, 100}}};
  const auto report = matching_engine::measurement::run_self_check(
      {&clock, &decltype(clock)::next}, {&steady, &decltype(steady)::next}, kRdtscp,
      SelfCheckConfig{.samples = 1, .calibration_windows = 1});

  EXPECT_FALSE(report.clock_safe);
  EXPECT_FALSE(report.calibrated);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::calibration_failure);
  EXPECT_EQ(steady.position, 2U);
}

TEST(MeasurementCalibration, UnstableCalibrationNeverClaimsCalibrated) {
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

  EXPECT_FALSE(report.clock_safe);
  EXPECT_FALSE(report.calibrated);
  EXPECT_EQ(report.self_check_reason, SelfCheckReason::calibration_instability);
}

TEST(ClockProbeMetadata, UsesOnlyFixedJsonSafeCompilerFields) {
  const auto metadata = matching_engine::measurement::compiler_metadata();
  const std::string_view family =
      matching_engine::measurement::compiler_family_name(metadata.family);

  EXPECT_FALSE(family.empty());
  for (const char character : family) {
    EXPECT_TRUE((character >= 'a' && character <= 'z') || character == '_');
  }
  EXPECT_GT(metadata.major, 0U);
}

} // namespace
