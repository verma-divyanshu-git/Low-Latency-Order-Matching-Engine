#include "support/differential_simulator.hpp"
#include "support/reference_order_book.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <tuple>

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

TEST(OrderBookDifferentialTest, ThresholdProRataMatchesIndependentReferenceModel) {
  constexpr std::array<std::uint64_t, 3> seeds{
      0x6a09e667f3bcc909ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL};
  for (const std::uint64_t seed : seeds) {
    DifferentialSimulator{Scenario::normal(), AllocationMode::threshold_pro_rata, Quantity{2U}}
        .run(seed, 1000U);
  }
}

TEST(OrderBookDifferentialTest, OpeningCrossMatchesIndependentReferenceModel) {
  OrderBook production{PriceDomain{Price{98}, 7U}, 8U, Quantity{100U}, SelfTradePolicy::none,
                       AllocationMode::fifo, Quantity{2U}, TradingState::opening_auction};
  ReferenceOrderBook reference{Price{98}, 7U, 8U, Quantity{100U}, SelfTradePolicy::none,
                               AllocationMode::fifo, Quantity{2U},
                               TradingState::opening_auction};
  std::array<Trade, 8U> production_trades{};
  const std::array orders{
      std::tuple{OrderId{1U}, Side::buy, Price{103}, Quantity{6U}},
      std::tuple{OrderId{2U}, Side::buy, Price{101}, Quantity{4U}},
      std::tuple{OrderId{3U}, Side::sell, Price{99}, Quantity{5U}},
      std::tuple{OrderId{4U}, Side::sell, Price{101}, Quantity{3U}},
      std::tuple{OrderId{5U}, Side::sell, Price{104}, Quantity{7U}},
  };
  for (const auto& [id, side, price, quantity] : orders) {
    const SubmitResult actual = production.submit_limit(id, side, price, quantity,
                                                        production_trades);
    const ModelSubmitResult expected = reference.submit_limit(
        id, side, price, quantity, TimeInForce::gtc, production_trades.size());
    EXPECT_EQ(actual.reject_reason, expected.reject_reason);
    EXPECT_EQ(actual.executed_quantity, expected.executed_quantity);
    EXPECT_EQ(actual.unfilled_quantity, expected.unfilled_quantity);
  }

  const AuctionResult actual = production.uncross_opening_auction(production_trades);
  const ModelAuctionResult expected =
      reference.uncross_opening_auction(production_trades.size());

  EXPECT_EQ(actual, expected.result);
  EXPECT_TRUE(std::equal(expected.trades.begin(), expected.trades.end(),
                         production_trades.begin()));
  EXPECT_EQ(production.best_bid(), reference.best_bid());
  EXPECT_EQ(production.best_ask(), reference.best_ask());
  EXPECT_EQ(production.level_info(Side::buy, Price{101}),
            reference.level_info(Side::buy, Price{101}));
  EXPECT_EQ(production.level_info(Side::sell, Price{104}),
            reference.level_info(Side::sell, Price{104}));
  EXPECT_EQ(production.last_execution_price(), reference.last_execution_price());
  EXPECT_EQ(production.check_invariants().violation, InvariantViolation::none);
}

TEST(OrderBookDifferentialSyntheticStressTest, HighCancelWorkload) {
  DifferentialSimulator{Scenario::high_cancel()}.run(0xfeedfacecafebeefULL, 1500U);
}

TEST(OrderBookDifferentialSyntheticStressTest, VolatilityShockWorkload) {
  DifferentialSimulator{Scenario::volatility_shock()}.run(0x9e3779b97f4a7c15ULL, 1500U);
}

TEST(OrderBookDifferentialTest, PostOnlyMatchesReferenceForCrossingAndRestingOrders) {
  OrderBook production{PriceDomain{Price{100}, 5U}, 4U, Quantity{10U}};
  ReferenceOrderBook reference{Price{100}, 5U, 4U, Quantity{10U}};
  std::array<Trade, 4U> production_trades{};

  ASSERT_EQ(production.submit_limit(OrderId{1U}, Side::sell, Price{102}, Quantity{4U},
                  production_trades)
        .reject_reason,
      RejectReason::none);
  ASSERT_EQ(reference.submit_limit(OrderId{1U}, Side::sell, Price{102}, Quantity{4U},
                   TimeInForce::gtc, 4U)
        .reject_reason,
      RejectReason::none);

  const SubmitResult production_crossing =
    production.submit_post_only(OrderId{2U}, Side::buy, Price{102}, Quantity{3U}, production_trades);
  const ModelSubmitResult reference_crossing =
    reference.submit_post_only(OrderId{2U}, Side::buy, Price{102}, Quantity{3U}, 4U);
  EXPECT_EQ(production_crossing.reject_reason, reference_crossing.reject_reason);
  EXPECT_EQ(production_crossing.executed_quantity, reference_crossing.executed_quantity);
  EXPECT_EQ(production_crossing.unfilled_quantity, reference_crossing.unfilled_quantity);
  EXPECT_EQ(production_crossing.trade_count, reference_crossing.trades.size());

  const SubmitResult production_resting =
    production.submit_post_only(OrderId{3U}, Side::buy, Price{101}, Quantity{3U}, production_trades);
  const ModelSubmitResult reference_resting =
    reference.submit_post_only(OrderId{3U}, Side::buy, Price{101}, Quantity{3U}, 4U);
  EXPECT_EQ(production_resting.reject_reason, reference_resting.reject_reason);
  EXPECT_EQ(production_resting.executed_quantity, reference_resting.executed_quantity);
  EXPECT_EQ(production_resting.unfilled_quantity, reference_resting.unfilled_quantity);
  EXPECT_EQ(production_resting.trade_count, reference_resting.trades.size());
  EXPECT_EQ(production.level_info(Side::buy, Price{101}), reference.level_info(Side::buy, Price{101}));
  EXPECT_EQ(production.level_info(Side::sell, Price{102}), reference.level_info(Side::sell, Price{102}));
  EXPECT_EQ(production.check_invariants().violation, InvariantViolation::none);
}

  TEST(OrderBookDifferentialTest, IcebergReplenishmentLosesPriorityAtItsPriceLevel) {
    OrderBook production{PriceDomain{Price{100}, 5U}, 8U, Quantity{20U}};
    ReferenceOrderBook reference{Price{100}, 5U, 8U, Quantity{20U}};
    std::array<Trade, 8U> production_trades{};

    const SubmitResult production_iceberg = production.submit_iceberg(
      OrderId{1U}, TraderId{7U}, Side::sell, Price{102}, Quantity{9U}, Quantity{3U},
      production_trades);
    const ModelSubmitResult reference_iceberg = reference.submit_iceberg(
      OrderId{1U}, TraderId{7U}, Side::sell, Price{102}, Quantity{9U}, Quantity{3U}, 8U);
    ASSERT_EQ(production_iceberg.reject_reason, reference_iceberg.reject_reason);
    ASSERT_EQ(production.submit_limit(OrderId{2U}, Side::sell, Price{102}, Quantity{2U},
                    production_trades)
          .reject_reason,
        RejectReason::none);
    ASSERT_EQ(reference.submit_limit(OrderId{2U}, Side::sell, Price{102}, Quantity{2U},
                     TimeInForce::gtc, 8U)
          .reject_reason,
        RejectReason::none);

    const SubmitResult production_fill = production.submit_market(
      OrderId{3U}, Side::buy, Quantity{8U}, production_trades);
    const ModelSubmitResult reference_fill =
      reference.submit_market(OrderId{3U}, Side::buy, Quantity{8U}, 8U);

    ASSERT_EQ(production_fill.trade_count, 2U);
    EXPECT_EQ(production_fill.executed_quantity, reference_fill.executed_quantity);
    EXPECT_EQ(production_fill.unfilled_quantity, reference_fill.unfilled_quantity);
    EXPECT_TRUE(std::equal(production_trades.begin(),
                           production_trades.begin() + production_fill.trade_count,
                           reference_fill.trades.begin(), reference_fill.trades.end()));
    EXPECT_EQ(production_trades[0].sell_id, OrderId{1U});
    EXPECT_EQ(production_trades[1].sell_id, OrderId{2U});
    EXPECT_EQ(production_trades[0].quantity, Quantity{6U});
    EXPECT_EQ(production_trades[1].quantity, Quantity{2U});
    EXPECT_EQ(production.level_info(Side::sell, Price{102}),
        reference.level_info(Side::sell, Price{102}));
    EXPECT_EQ(production.check_invariants().violation, InvariantViolation::none);
  }

  TEST(OrderBookDifferentialTest, IcebergValidatesQuantityBeforeTradeCapacity) {
    OrderBook production{PriceDomain{Price{100}, 5U}, 8U, Quantity{16U}};
    ReferenceOrderBook reference{Price{100}, 5U, 8U, Quantity{16U}};
    std::array<Trade, 1U> production_trades{};

    const SubmitResult production_result = production.submit_iceberg(
        OrderId{1U}, Side::sell, Price{102}, Quantity{17U}, Quantity{10U}, production_trades);
    const ModelSubmitResult reference_result = reference.submit_iceberg(
        OrderId{1U}, TraderId{0U}, Side::sell, Price{102}, Quantity{17U}, Quantity{10U},
        production_trades.size());

    EXPECT_EQ(production_result.reject_reason, RejectReason::quantity_too_large);
    EXPECT_EQ(production_result.reject_reason, reference_result.reject_reason);
    EXPECT_EQ(production.check_invariants().violation, InvariantViolation::none);
  }

} // namespace
} // namespace matching_engine::test
