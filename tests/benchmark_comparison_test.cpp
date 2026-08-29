#include "matching_engine/benchmark_comparison.hpp"

#include <gtest/gtest.h>

namespace matching_engine::benchmark_comparison {

TEST(BenchmarkComparisonTest, EveryImplementationProducesIdenticalChecksum) {
  Config config{.active_levels = 16U, .operations = 128U, .repetitions = 3U};
  std::string error;
  const auto report = run(config, error);
  ASSERT_TRUE(report.has_value()) << error;
  ASSERT_EQ(report->results.size(), 4U);
  for (const Result& result : report->results) {
    EXPECT_EQ(result.checksum, report->results.front().checksum);
    EXPECT_EQ(result.elapsed_ns.size(), 3U);
    EXPECT_GT(result.median_operations_per_second, 0.0);
  }
}

TEST(BenchmarkComparisonTest, RejectsInvalidAndNoncanonicalCliValues) {
  EXPECT_FALSE(parse_cli({"--active-levels", "0"}).has_value());
  EXPECT_FALSE(parse_cli({"--operations", "01"}).has_value());
  EXPECT_FALSE(parse_cli({"--repetitions", "2"}).has_value());
  EXPECT_FALSE(parse_cli({"--output", "../result.json"}).has_value());
  EXPECT_FALSE(parse_cli({"--wat", "1"}).has_value());
  EXPECT_FALSE(parse_cli({"--active-levels", "600000", "--operations", "500000"}).has_value());
}

TEST(BenchmarkComparisonTest, JsonIsRegressionOnlyAndContainsNoLatencyClaim) {
  Config config{.active_levels = 8U, .operations = 32U, .repetitions = 3U};
  std::string error;
  const auto report = run(config, error);
  ASSERT_TRUE(report.has_value()) << error;
  const auto json = report_json(*report);
  ASSERT_TRUE(json.has_value());
  EXPECT_NE(json->find("\"claim_scope\":\"regression_only\""), std::string::npos);
  EXPECT_NE(json->find("\"metric\":\"batch_amortized_mean\""), std::string::npos);
  EXPECT_EQ(json->find("percentile"), std::string::npos);
  EXPECT_EQ(json->find("latency"), std::string::npos);
}

} // namespace matching_engine::benchmark_comparison
