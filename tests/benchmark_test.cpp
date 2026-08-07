#include "matching_engine/benchmark.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace matching_engine::benchmark {
namespace {

struct FakeClock {
  std::vector<measurement::ClockSample> samples;
  std::size_t index{};

  static measurement::ClockSample read(void* context) noexcept {
    auto& clock = *static_cast<FakeClock*>(context);
    const auto sample = clock.samples[clock.index];
    ++clock.index;
    return sample;
  }
};

TEST(BenchmarkScheduleTest, UsesExactRationalOffsetsWithoutDrift) {
  const auto schedule = make_schedule(4U, 3U, TickRatio{.numerator = 2U, .denominator = 1U});

  if (!schedule.has_value()) {
    FAIL() << "expected representable schedule";
    return;
  }
  EXPECT_EQ(schedule.value(),
            (std::vector<std::uint64_t>{0U, 666'666'666U, 1'333'333'333U, 2'000'000'000U}));
}

TEST(BenchmarkScheduleTest, RejectsOverflowAndZeroInputs) {
  EXPECT_FALSE(make_schedule(0U, 1U, TickRatio{1U, 1U}).has_value());
  EXPECT_FALSE(make_schedule(1U, 0U, TickRatio{1U, 1U}).has_value());
  EXPECT_FALSE(
      make_schedule(2U, 1U, TickRatio{std::numeric_limits<std::uint64_t>::max(), 1U}).has_value());
  EXPECT_FALSE(make_schedule(2U, 1U, TickRatio{1U, 0U}).has_value());
}

TEST(BenchmarkObservationTest, LateCompletionIncludesQueueingDelay) {
  const auto observation = observe_event(
      measurement::ClockSample{100U, 7U}, measurement::ClockSample{130U, 7U},
      measurement::ClockSample{145U, 7U},
      measurement::ClockCapabilities{measurement::ClockSourceKind::x86_rdtscp, true, true, true});

  ASSERT_EQ(observation.status, measurement::ElapsedStatus::valid);
  EXPECT_EQ(observation.lateness_ticks, 30U);
  EXPECT_EQ(observation.service_ticks, 15U);
  EXPECT_EQ(observation.latency_ticks, 45U);
}

TEST(BenchmarkObservationTest, FakeClockWaitsOnlyWhenEarlyAndNeverReschedulesLateEvent) {
  const measurement::ClockCapabilities capabilities{measurement::ClockSourceKind::steady_clock_ns,
                                                    true, false, false};
  FakeClock early{{{90U, 0U}, {99U, 0U}, {105U, 0U}}};
  const auto early_start =
      wait_until_intended({&early, &FakeClock::read}, {100U, 0U}, capabilities);
  EXPECT_EQ(early_start.ticks, 105U);
  EXPECT_EQ(early.index, 3U);

  FakeClock late{{{130U, 0U}}};
  const auto late_start = wait_until_intended({&late, &FakeClock::read}, {100U, 0U}, capabilities);
  EXPECT_EQ(late_start.ticks, 130U);
  EXPECT_EQ(late.index, 1U);
}

TEST(BenchmarkObservationTest, RejectsBackwardAndMigratedSamples) {
  const measurement::ClockCapabilities capabilities{measurement::ClockSourceKind::x86_rdtscp, true,
                                                    true, true};
  EXPECT_EQ(observe_event({100U, 1U}, {99U, 1U}, {101U, 1U}, capabilities).status,
            measurement::ElapsedStatus::backward);
  EXPECT_EQ(observe_event({100U, 1U}, {101U, 2U}, {102U, 2U}, capabilities).status,
            measurement::ElapsedStatus::migrated);
}

TEST(BenchmarkHistogramTest, RawRecordingNeverAppliesCorrection) {
  Histogram histogram{1U, 1'000'000U, 3};
  ASSERT_TRUE(histogram.valid());
  ASSERT_TRUE(histogram.record(100U));
  ASSERT_TRUE(histogram.record(1'000U));

  EXPECT_EQ(histogram.count(), 2U);
}

TEST(BenchmarkHistogramTest, ExposesRecordedBucketsAndKnownPercentiles) {
  Histogram histogram{1U, 10'000U, 3};
  for (const std::uint64_t value : {10U, 10U, 20U, 40U}) {
    ASSERT_TRUE(histogram.record(value));
  }

  EXPECT_EQ(histogram.percentile(50.0), 10U);
  EXPECT_EQ(histogram.percentile(90.0), 40U);
  EXPECT_EQ(histogram.recorded_buckets(), (std::vector<Bucket>{{.value = 10U, .count = 2U},
                                                               {.value = 20U, .count = 1U},
                                                               {.value = 40U, .count = 1U}}));
}

TEST(BenchmarkScenarioTest, CrossingAndSweepValidateTradeShapeAndChecksum) {
  const auto crossing = exercise_scenario(Scenario::crossing_limit, 3U);
  ASSERT_TRUE(crossing.valid);
  EXPECT_EQ(crossing.trade_count, 3U);
  EXPECT_NE(crossing.checksum, 0U);

  const auto sweep = exercise_scenario(Scenario::sweep_3_level, 2U);
  ASSERT_TRUE(sweep.valid);
  EXPECT_EQ(sweep.trade_count, 6U);
  EXPECT_NE(sweep.checksum, crossing.checksum);
}

TEST(BenchmarkDiagnosticTest, CorrectionAddsSyntheticOmittedSamplesAndTail) {
  const auto diagnostic = synthetic_diagnostic({10U, 10U, 100U}, 10U);

  EXPECT_EQ(diagnostic.raw.count(), 3U);
  EXPECT_GT(diagnostic.corrected.count(), diagnostic.raw.count());
  EXPECT_GT(diagnostic.corrected.percentile(90.0), diagnostic.raw.percentile(50.0));
}

TEST(BenchmarkCliTest, RejectsMalformedZeroTrailingAndImpossibleArguments) {
  const auto reject = [](std::initializer_list<const char*> arguments) {
    std::vector<std::string> owned;
    for (const char* argument : arguments) {
      owned.emplace_back(argument);
    }
    return !parse_cli(owned).has_value();
  };

  EXPECT_TRUE(reject({"--samples", "0"}));
  EXPECT_TRUE(reject({"--samples", "1000001"}));
  EXPECT_TRUE(reject({"--rate", "10x"}));
  EXPECT_TRUE(reject({"--mode", "open-loop", "--diagnostic-stall-ns", "10"}));
  EXPECT_TRUE(reject({"--mode", "closed-loop-diagnostic", "--scenario", "crossing-limit"}));
  EXPECT_TRUE(reject({"--mode", "closed-loop-diagnostic", "--rate", "10"}));
  EXPECT_TRUE(reject({"--mode", "closed-loop-diagnostic", "--warmup", "10"}));
  EXPECT_TRUE(reject({"--samples", "10", "--samples", "20"}));
  EXPECT_TRUE(reject({"--rate", "1000000001"}));
  EXPECT_TRUE(
      reject({"--mode", "closed-loop-diagnostic", "--diagnostic-stall-ns", "3600000000001"}));
  EXPECT_TRUE(reject({"--output-dir", ""}));
  EXPECT_TRUE(reject({"--unknown", "1"}));
}

TEST(BenchmarkJsonTest, EmitsStrictRequiredFieldsWithoutEnvironmentIdentity) {
  Summary summary{};
  summary.mode = Mode::open_loop;
  summary.scenario = Scenario::crossing_limit;
  summary.count = 2U;
  summary.claim_scope = ClaimScope::regression_only;
  summary.publication_reason = "source_regression_only";
  const std::string json = summary_json(summary);

  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  EXPECT_NE(json.find("\"claim_scope\":\"regression_only\""), std::string::npos);
  EXPECT_NE(json.find("\"clock_report\":"), std::string::npos);
  EXPECT_EQ(json.find("hostname"), std::string::npos);
  EXPECT_EQ(json.find("username"), std::string::npos);
  EXPECT_EQ(json.find(",}"), std::string::npos);
}

} // namespace
} // namespace matching_engine::benchmark
