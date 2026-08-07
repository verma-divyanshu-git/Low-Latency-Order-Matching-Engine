#include "matching_engine/benchmark.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <span>
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

struct MutableClock {
  std::uint64_t ticks{100U};
  std::uint32_t auxiliary{1U};

  static measurement::ClockSample read(void* context) noexcept {
    const auto& clock = *static_cast<MutableClock*>(context);
    return {clock.ticks, clock.auxiliary};
  }
};

struct TimedOperation {
  MutableClock* clock{};
  std::uint64_t validation_delay{};

  static void submit(void* context, std::uint64_t) noexcept {
    auto& operation = *static_cast<TimedOperation*>(context);
    operation.clock->ticks += 10U;
  }

  static bool validate(void* context, std::uint64_t) noexcept {
    auto& operation = *static_cast<TimedOperation*>(context);
    operation.clock->ticks += operation.validation_delay;
    return true;
  }
};

[[nodiscard]] std::filesystem::path unique_test_path(std::string_view suffix) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("matching-engine-benchmark-" + std::to_string(ticks) + "-" + std::string{suffix});
}

TEST(BenchmarkScheduleTest, UsesNearestRationalOffsetsWithoutDrift) {
  const auto schedule = make_schedule(4U, 3U, TickRatio{.numerator = 2U, .denominator = 1U});

  if (!schedule.has_value()) {
    FAIL() << "expected representable schedule";
    return;
  }
  EXPECT_EQ(schedule.value(),
            (std::vector<std::uint64_t>{0U, 666'666'667U, 1'333'333'333U, 2'000'000'000U}));
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
  const measurement::ClockCapabilities capabilities{measurement::ClockSourceKind::x86_rdtscp, true,
                                                    true, true};
  FakeClock early{{{90U, 1U}, {99U, 2U}, {105U, 2U}}};
  const auto early_start =
      wait_until_intended({&early, &FakeClock::read}, {100U, 1U}, capabilities);
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
            measurement::ElapsedStatus::valid);
  EXPECT_EQ(observe_event({100U, 1U}, {101U, 2U}, {102U, 3U}, capabilities).status,
            measurement::ElapsedStatus::migrated);
}

TEST(BenchmarkRunnerTest, PersistentMigrationDiscardsOnlyCrossCpuEventAndContinuesSchedule) {
  const measurement::ClockCapabilities capabilities{measurement::ClockSourceKind::x86_rdtscp, true,
                                                    true, true};
  FakeClock clock{{{100U, 1U}, {110U, 2U}, {115U, 2U}, {125U, 2U}, {130U, 2U}}};
  MutableClock unused_clock{};
  TimedOperation operation{&unused_clock, 0U};
  Histogram latency{1U, 1'000'000U, 3};
  Histogram service{1U, 1'000'000U, 3};
  OpenLoopStats stats{};
  const std::array<std::uint64_t, 2> schedule{0U, 20U};

  ASSERT_TRUE(collect_open_loop(schedule, {100U, 1U}, {&clock, &FakeClock::read}, capabilities, 1.0,
                                {&operation, &TimedOperation::submit, &TimedOperation::validate},
                                latency, service, stats));
  EXPECT_EQ(stats.executed_operations, 2U);
  EXPECT_EQ(stats.migration_samples, 1U);
  EXPECT_EQ(stats.valid_samples, 1U);
  EXPECT_EQ(clock.index, 5U);
}

TEST(BenchmarkRunnerTest, CompletionTimestampExcludesMandatoryValidationDelay) {
  const measurement::ClockCapabilities capabilities{measurement::ClockSourceKind::steady_clock_ns,
                                                    true, false, false};
  MutableClock clock{};
  TimedOperation operation{&clock, 1'000U};
  Histogram latency{1U, 1'000'000U, 3};
  Histogram service{1U, 1'000'000U, 3};
  OpenLoopStats stats{};
  const std::array<std::uint64_t, 1> schedule{0U};

  ASSERT_TRUE(collect_open_loop(
      schedule, {100U, 0U}, {&clock, &MutableClock::read}, capabilities, 1.0,
      {&operation, &TimedOperation::submit, &TimedOperation::validate}, latency, service, stats));
  EXPECT_EQ(service.percentile(50.0), 10U);
  EXPECT_EQ(latency.percentile(50.0), 10U);
  EXPECT_EQ(clock.ticks, 1'110U);
}

TEST(BenchmarkRunnerTest, BacklogExcludesCurrentlyStartingEvent) {
  const std::array<std::uint64_t, 4> schedule{0U, 10U, 20U, 30U};
  EXPECT_EQ(additional_backlog(schedule, 0U, 0U), 0U);
  EXPECT_EQ(additional_backlog(schedule, 1U, 10U), 0U);
  EXPECT_EQ(additional_backlog(schedule, 1U, 25U), 1U);
}

TEST(BenchmarkRunnerTest, AchievedCompletionRateUsesExecutedOperationsAndNMinusOneIntervals) {
  EXPECT_DOUBLE_EQ(achieved_completion_rate(3U, 100U, 300U, 1.0), 10'000'000.0);
  EXPECT_DOUBLE_EQ(achieved_completion_rate(1U, 100U, 100U, 1.0), 0.0);
}

TEST(BenchmarkRunnerTest, OperationResolutionIsEvaluatedWithoutSourceQualification) {
  const auto evaluation = evaluate_operation_resolution(41U, 42U);
  EXPECT_EQ(evaluation.threshold_ticks, 410U);
  EXPECT_FALSE(evaluation.resolved);
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

TEST(BenchmarkHistogramTest, EmitsFullHdrPercentileIteration) {
  Histogram histogram{1U, 10'000U, 3};
  for (std::uint64_t value = 1U; value <= 100U; ++value) {
    ASSERT_TRUE(histogram.record(value));
  }

  const auto distribution = histogram.percentile_distribution();
  EXPECT_GT(distribution.size(), 7U);
  EXPECT_EQ(distribution.back().cumulative_count, 100U);
  EXPECT_DOUBLE_EQ(distribution.back().percentile, 100.0);
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

TEST(BenchmarkDiagnosticTest, BoundsCorrectionIterationsBeforeRecording) {
  std::uint64_t corrected_count{};
  EXPECT_TRUE(
      diagnostic_correction_count_upper_bound(100U, 10'000U, 10U, 1'000'000U, corrected_count));
  EXPECT_EQ(corrected_count, 1'090U);
  EXPECT_FALSE(diagnostic_correction_count_upper_bound(1'000'000U, 1U, 1U, 3'600'000'000'000U,
                                                       corrected_count));
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
  EXPECT_TRUE(reject({"--mode", "closed-loop-diagnostic", "--samples", "1000000",
                      "--diagnostic-interval-ns", "1", "--diagnostic-stall-every", "1",
                      "--diagnostic-stall-ns", "3600000000000"}));
  EXPECT_TRUE(reject({"--mode", "open-loop", "--scenario", "sweep-3-level", "--samples", "1000000",
                      "--warmup", "1"}));
  EXPECT_TRUE(reject({"--output-dir", ""}));
  EXPECT_TRUE(reject({"--unknown", "1"}));
}

TEST(BenchmarkJsonTest, EmitsStrictRequiredFieldsWithoutEnvironmentIdentity) {
  Summary summary{};
  summary.mode = Mode::open_loop;
  summary.scenario = Scenario::crossing_limit;
  summary.count = 2U;
  summary.executed_operations = 3U;
  summary.claim_scope = ClaimScope::regression_only;
  summary.source_qualification_reason = "source_regression_only";
  summary.operation_resolution_reason = "operation_below_resolution";
  summary.operation_median_ticks = 42U;
  summary.operation_resolution_threshold_ticks = 410U;
  const std::string json = summary_json(summary);

  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  EXPECT_NE(json.find("\"claim_scope\":\"regression_only\""), std::string::npos);
  EXPECT_NE(json.find("\"clock_report\":"), std::string::npos);
  EXPECT_NE(json.find("\"executed_operations\":3"), std::string::npos);
  EXPECT_NE(json.find("\"source_qualification_reason\":\"source_regression_only\""),
            std::string::npos);
  EXPECT_NE(json.find("\"operation_resolution_reason\":\"operation_below_resolution\""),
            std::string::npos);
  EXPECT_NE(json.find("\"effective_granularity_ns\":"), std::string::npos);
  EXPECT_EQ(json.find("hostname"), std::string::npos);
  EXPECT_EQ(json.find("username"), std::string::npos);
  EXPECT_EQ(json.find(",}"), std::string::npos);
}

TEST(BenchmarkJsonTest, DiagnosticIsTopLevelOnlyAndLeavesClockOperationUnevaluated) {
  Summary summary{};
  summary.mode = Mode::closed_loop_diagnostic;
  summary.claim_scope = ClaimScope::diagnostic_only;
  summary.clock_report.operation_evaluated = false;
  summary.clock_report.operation_percentiles_publishable = false;
  summary.clock_report.publication_reason = measurement::PublicationReason::operation_not_evaluated;
  const std::string json = summary_json(summary);

  EXPECT_NE(json.find("\"claim_scope\":\"diagnostic_only\""), std::string::npos);
  EXPECT_NE(json.find("\"operation_evaluated\":false"), std::string::npos);
  EXPECT_NE(json.find("\"publication_reason\":\"operation_not_evaluated\""), std::string::npos);
}

TEST(BenchmarkMemoryTest, ReportsPlanAndRejectsRunsAboveCiBudget) {
  const auto small = benchmark_memory_plan(Scenario::crossing_limit, 100U, 10U);
  if (!small.has_value()) {
    FAIL() << "expected representable memory plan";
    return;
  }
  EXPECT_GT(small.value().planned_bytes, 0U);
  EXPECT_LE(small.value().planned_bytes, kBenchmarkMemoryBudgetBytes);
  EXPECT_FALSE(benchmark_memory_plan(Scenario::sweep_3_level, 1'000'000U, 1U).has_value());
}

TEST(BenchmarkArtifactTest, RejectsExistingFinalDirectoryWithoutOverwriting) {
  const auto output = unique_test_path("existing");
  std::error_code error_code;
  ASSERT_TRUE(
      std::filesystem::create_directories(output / "open-loop-crossing-limit-run", error_code));
  Config config{};
  config.samples = 2U;
  config.warmup = 1U;
  config.rate = 1'000'000'000U;
  config.output_dir = output;
  std::string error;

  EXPECT_FALSE(run(config, error).has_value());
  EXPECT_NE(error.find("already exists"), std::string::npos);
  std::filesystem::remove_all(output, error_code);
}

TEST(BenchmarkArtifactTest, ReportsOutputPathFailureWithoutArtifacts) {
  const auto output = unique_test_path("file");
  {
    std::ofstream file(output);
    ASSERT_TRUE(file.good());
  }
  Config config{};
  config.samples = 2U;
  config.warmup = 1U;
  config.rate = 1'000'000'000U;
  config.output_dir = output;
  std::string error;

  EXPECT_FALSE(run(config, error).has_value());
  EXPECT_NE(error.find("output"), std::string::npos);
  std::error_code error_code;
  std::filesystem::remove(output, error_code);
}

TEST(BenchmarkArtifactTest, PublishesCompleteRunAsOneFinalDirectory) {
  const auto output = unique_test_path("complete");
  Config config{};
  config.samples = 2U;
  config.warmup = 1U;
  config.rate = 100U;
  config.output_dir = output;
  std::string error;

  const auto result = run(config, error);
  if (!result.has_value()) {
    FAIL() << error;
    return;
  }
  EXPECT_EQ(result.value().summary.executed_operations, 2U);
  EXPECT_EQ(result.value().summary.max_backlog, 0U);
  EXPECT_EQ(result.value().summary_path.parent_path(), result.value().final_directory);
  EXPECT_TRUE(std::filesystem::exists(result.value().summary_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().raw_csv_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().percentile_path));
  for (const auto& entry : std::filesystem::directory_iterator(output)) {
    EXPECT_FALSE(entry.path().filename().string().starts_with(".open-loop"));
  }
  std::error_code error_code;
  std::filesystem::remove_all(output, error_code);
}

} // namespace
} // namespace matching_engine::benchmark
