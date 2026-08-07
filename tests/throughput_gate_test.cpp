#include "matching_engine/throughput_gate.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace matching_engine::throughput_gate {
namespace {

TEST(ThroughputConfigTest, DefaultFloorMatchesCiPolicy) {
  EXPECT_DOUBLE_EQ(Config{}.minimum_ops_per_second, 1'000'000.0);
}

TEST(ThroughputStatisticsTest, ComputesOddAndEvenMedianWithoutOverflow) {
  EXPECT_EQ(median({9U, 1U, 5U}), 5U);
  EXPECT_EQ(median({10U, 2U, 8U, 4U}), 6U);
  EXPECT_EQ(median({std::numeric_limits<std::uint64_t>::max(),
                    std::numeric_limits<std::uint64_t>::max()}),
            std::numeric_limits<std::uint64_t>::max());
}

TEST(ThroughputStatisticsTest, ComputesMinimumMedianMadAndRelativeMad) {
  const auto result = summarize_durations({100U, 120U, 140U}, 10U);
  if (!result.has_value()) {
    FAIL() << "expected valid statistics";
    return;
  }
  EXPECT_EQ(result.value().best_elapsed_ns, 100U);
  EXPECT_EQ(result.value().median_elapsed_ns, 120U);
  EXPECT_EQ(result.value().median_absolute_deviation_ns, 20U);
  EXPECT_DOUBLE_EQ(result.value().relative_mad, 1.0 / 6.0);
  EXPECT_DOUBLE_EQ(result.value().best_ops_per_second, 100'000'000.0);
}

TEST(ThroughputStatisticsTest, RejectsZeroDurationEmptyInputAndUnrepresentableSamples) {
  EXPECT_FALSE(summarize_durations({}, 1U).has_value());
  EXPECT_FALSE(summarize_durations({0U}, 1U).has_value());
  EXPECT_FALSE(summarize_durations({1U}, std::numeric_limits<std::uint64_t>::max()).has_value());
}

TEST(ThroughputGateTest, ThresholdBoundariesAreInclusive) {
  Statistics statistics{};
  statistics.validation_passed = true;
  statistics.best_ops_per_second = 1'000.0;
  statistics.relative_mad = 0.25;
  EXPECT_TRUE(passes_gate(statistics, 1'000.0, 0.25));
  EXPECT_FALSE(passes_gate(statistics, 1'000.01, 0.25));
  EXPECT_FALSE(passes_gate(statistics, 1'000.0, 0.249));
  statistics.validation_passed = false;
  EXPECT_FALSE(passes_gate(statistics, 0.0, 1.0));
}

TEST(ThroughputJsonTest, EmitsStrictFiniteRegressionOnlySchema) {
  Statistics statistics{};
  statistics.samples = 10U;
  statistics.repetitions = 3U;
  statistics.elapsed_ns = {100U, 120U, 140U};
  statistics.best_elapsed_ns = 100U;
  statistics.median_elapsed_ns = 120U;
  statistics.median_absolute_deviation_ns = 20U;
  statistics.relative_mad = 1.0 / 6.0;
  statistics.best_ops_per_second = 100'000'000.0;
  statistics.validation_passed = true;
  statistics.checksum = 42U;
  statistics.gate_passed = true;
  const auto json = statistics_json(statistics, 1'000.0, 0.25);
  if (!json.has_value()) {
    FAIL() << "expected valid JSON";
    return;
  }
  EXPECT_NE(json.value().find("\"claim_scope\":\"ci_regression_only\""), std::string::npos);
  EXPECT_NE(json.value().find("\"metric\":\"batch_amortized_mean\""), std::string::npos);
  EXPECT_EQ(json.value().find("hostname"), std::string::npos);
  EXPECT_EQ(json.value().find("percentile"), std::string::npos);

  statistics.relative_mad = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(statistics_json(statistics, 1'000.0, 0.25).has_value());
}

TEST(ThroughputCliTest, RejectsMalformedOutOfRangeAndTrailingArguments) {
  EXPECT_FALSE(parse_cli({"--samples", "0"}).has_value());
  EXPECT_FALSE(parse_cli({"--repetitions", "2"}).has_value());
  EXPECT_FALSE(parse_cli({"--repetitions", "22"}).has_value());
  EXPECT_FALSE(parse_cli({"--samples", "10x"}).has_value());
  EXPECT_FALSE(parse_cli({"--min-ops-per-second", "0"}).has_value());
  EXPECT_FALSE(parse_cli({"--max-relative-mad", "nan"}).has_value());
  EXPECT_FALSE(parse_cli({"--output", "../result.json"}).has_value());
  EXPECT_FALSE(parse_cli({"trailing"}).has_value());
}

TEST(ThroughputGateTest, RejectsInvalidDirectThresholds) {
  Statistics statistics{};
  statistics.validation_passed = true;
  statistics.best_ops_per_second = 1'000.0;
  statistics.relative_mad = 0.1;
  EXPECT_FALSE(passes_gate(statistics, 0.0, 0.25));
  EXPECT_FALSE(passes_gate(statistics, 1.0, -0.1));
}

TEST(ThroughputEndToEndTest, RunsSmallValidatedCrossingBatch) {
  Config config{};
  config.samples = 32U;
  config.repetitions = 3U;
  config.minimum_ops_per_second = 1.0;
  config.maximum_relative_mad = 1.0;
  std::string error;
  const auto result = run(config, error);
  if (!result.has_value()) {
    FAIL() << error;
    return;
  }
  EXPECT_TRUE(result.value().validation_passed);
  EXPECT_TRUE(result.value().gate_passed);
  EXPECT_EQ(result.value().elapsed_ns.size(), 3U);
  EXPECT_NE(result.value().checksum, 0U);
}

} // namespace
} // namespace matching_engine::throughput_gate
