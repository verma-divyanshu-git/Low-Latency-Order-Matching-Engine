#include "support/differential_simulator.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>

namespace matching_engine::test {
namespace {

[[nodiscard]] std::optional<std::uint64_t> replay_seed() {
  // Test discovery and execution are single-threaded when this immutable process setting is read.
  const char* const value = std::getenv("ORDER_BOOK_DIFF_SEED"); // NOLINT(concurrency-mt-unsafe)
  if (value == nullptr) {
    return std::nullopt;
  }
  std::uint64_t seed = 0U;
  const std::string_view text{value};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed, 10);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return seed;
}

TEST(OrderBookDifferentialTest, NormalSeedRegressionCorpusCoversTenThousandOperations) {
  constexpr std::array<std::uint64_t, 10> seeds{
      0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL, 0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL,
      0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL, 0xc0ac29b7c97c50ddULL, 0x3f84d5b5b5470917ULL,
      0x9216d5d98979fb1bULL, 0xd1310ba698dfb5acULL};
  const Scenario scenario = Scenario::normal();
  if (const auto seed = replay_seed(); seed.has_value()) {
    DifferentialSimulator{scenario}.run(*seed, 1000U);
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
