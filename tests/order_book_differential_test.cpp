#include "support/differential_simulator.hpp"

#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <optional>

namespace matching_engine::test {
namespace {

TEST(DifferentialSimulatorSupportTest, ParsesOnlyCompleteUnsignedDecimalSeeds) {
  EXPECT_EQ(parse_replay_seed("0"), 0U);
  EXPECT_EQ(parse_replay_seed("18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(parse_replay_seed(""), std::nullopt);
  EXPECT_EQ(parse_replay_seed("-1"), std::nullopt);
  EXPECT_EQ(parse_replay_seed("+1"), std::nullopt);
  EXPECT_EQ(parse_replay_seed("12x"), std::nullopt);
  EXPECT_EQ(parse_replay_seed(" 12"), std::nullopt);
  EXPECT_EQ(parse_replay_seed("18446744073709551616"), std::nullopt);
}

TEST(DifferentialSimulatorSupportTest, CheckedArithmeticRejectsUint64Overflow) {
  constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(checked_add(4U, 5U), 9U);
  EXPECT_EQ(checked_add(maximum, 1U), std::nullopt);
  EXPECT_EQ(checked_double(maximum / 2U), maximum - 1U);
  EXPECT_EQ(checked_double((maximum / 2U) + 1U), std::nullopt);
}

TEST(OrderBookDifferentialTest, NormalSeedRegressionCorpusCoversTenThousandOperations) {
  constexpr std::array<std::uint64_t, 10> seeds{
      0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL, 0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL,
      0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL, 0xc0ac29b7c97c50ddULL, 0x3f84d5b5b5470917ULL,
      0x9216d5d98979fb1bULL, 0xd1310ba698dfb5acULL};
  const Scenario scenario = Scenario::normal();
  // Test discovery and execution are single-threaded when this immutable process setting is read.
  const char* const replay = std::getenv("ORDER_BOOK_DIFF_SEED"); // NOLINT(concurrency-mt-unsafe)
  if (replay != nullptr) {
    const auto seed = parse_replay_seed(replay);
    ASSERT_TRUE(seed.has_value())
        << "ORDER_BOOK_DIFF_SEED must be a complete unsigned decimal uint64 value";
    DifferentialSimulator{scenario}.run(seed.value_or(0U), 1000U);
    return;
  }
  for (const std::uint64_t seed : seeds) {
    DifferentialSimulator{scenario}.run(seed, 1000U);
  }
}

TEST(OrderBookDifferentialSyntheticStressTest, HighCancelWorkload) {
  DifferentialSimulator{Scenario::high_cancel()}.run(0xfeedfacecafebeefULL, 1500U);
}

TEST(OrderBookDifferentialSyntheticStressTest, VolatilityShockWorkload) {
  DifferentialSimulator{Scenario::volatility_shock()}.run(0x9e3779b97f4a7c15ULL, 1500U);
}

} // namespace
} // namespace matching_engine::test
