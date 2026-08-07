#include "matching_engine/order_book.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace matching_engine {
namespace {

static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_trivially_copyable_v<SubmitResult>);
static_assert(noexcept(std::declval<OrderBook&>().submit_limit(OrderId{1}, Side::buy, Price{1},
                                                               Quantity{1},
                                                               std::declval<std::span<Trade>>())));
static_assert(noexcept(std::declval<OrderBook&>().submit_market(OrderId{1}, Side::buy, Quantity{1},
                                                                std::declval<std::span<Trade>>())));
static_assert(is_valid_side(Side::buy));
static_assert(is_valid_side(Side::sell));
static_assert(!is_valid_side(static_cast<Side>(2U)));

constexpr Handle kNoHandle{.index = kInvalidIndex, .generation = 0U};

class OrderBookTest : public ::testing::Test {
public:
  OrderBook book{PriceDomain{Price{100}, 11U}, 8U, Quantity{100U}};
  std::array<Trade, 8> trades{};

  [[nodiscard]] SubmitResult limit(std::uint64_t id, Side side, std::int64_t price,
                                   std::uint64_t quantity) {
    return book.submit_limit(OrderId{id}, side, Price{price}, Quantity{quantity}, trades);
  }

  [[nodiscard]] SubmitResult market(std::uint64_t id, Side side, std::uint64_t quantity) {
    return book.submit_market(OrderId{id}, side, Quantity{quantity}, trades);
  }

  static void expect_result(const SubmitResult& result, RejectReason reason, std::uint64_t executed,
                            std::uint64_t unfilled, std::uint32_t trade_count,
                            Handle handle = kNoHandle) {
    EXPECT_EQ(result.reject_reason, reason);
    EXPECT_EQ(result.executed_quantity, Quantity{executed});
    EXPECT_EQ(result.unfilled_quantity, Quantity{unfilled});
    EXPECT_EQ(result.trade_count, trade_count);
    EXPECT_EQ(result.resting_handle, handle);
  }

  static void expect_level(const OrderBook& target, Side side, std::int64_t price,
                           std::uint64_t quantity, std::uint32_t count) {
    const auto info = target.level_info(side, Price{price});
    if (!info.has_value()) {
      FAIL() << "expected in-domain level";
      return;
    }
    EXPECT_EQ(info->aggregate_quantity, Quantity{quantity});
    EXPECT_EQ(info->order_count, count);
  }
};

TEST(OrderEncodingTest, EncodesSideInHighBitAndLevelInRemainingBits) {
  EXPECT_EQ(detail::encode_level_side(0U, Side::buy), 0U);
  EXPECT_EQ(detail::encode_level_side(kOrderLevelMask, Side::buy), kOrderLevelMask);
  EXPECT_EQ(detail::encode_level_side(0U, Side::sell), kOrderSideMask);
  EXPECT_EQ(detail::encode_level_side(kOrderLevelMask, Side::sell),
            std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(detail::decode_level(detail::encode_level_side(kOrderLevelMask, Side::sell)),
            kOrderLevelMask);
  EXPECT_EQ(detail::decode_side(detail::encode_level_side(0U, Side::buy)), Side::buy);
  EXPECT_EQ(detail::decode_side(detail::encode_level_side(0U, Side::sell)), Side::sell);
}

TEST(OrderEncodingTest, ValidatesTickCountRatherThanMaximumIndex) {
  constexpr std::uint64_t maximum_tick_count = std::uint64_t{kOrderLevelMask} + 1U;

  EXPECT_FALSE(detail::is_encodable_tick_count(0U));
  EXPECT_TRUE(detail::is_encodable_tick_count(1U));
  EXPECT_TRUE(detail::is_encodable_tick_count(maximum_tick_count));
  EXPECT_FALSE(detail::is_encodable_tick_count(maximum_tick_count + 1U));
}

TEST(OrderBookConstructionTest, ValidatesConfigurationBoundaries) {
  EXPECT_THROW((OrderBook{PriceDomain{Price{0}, kOrderSideMask + 1U}, 1U, Quantity{1U}}),
               std::length_error);
  EXPECT_THROW((OrderBook{PriceDomain{Price{0}, 1U}, 1U, Quantity{0U}}), std::invalid_argument);
  EXPECT_THROW(
      (OrderBook{
          PriceDomain{Price{0}, 1U}, 1U,
          Quantity{static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U}}),
      std::invalid_argument);
}

TEST_F(OrderBookTest, PassiveOrdersRestAndPublishBboAndLevelAggregates) {
  const SubmitResult bid = limit(1, Side::buy, 103, 7);
  const SubmitResult second_bid = limit(2, Side::buy, 103, 5);
  const SubmitResult ask = limit(3, Side::sell, 107, 9);

  expect_result(bid, RejectReason::none, 0, 7, 0, bid.resting_handle);
  EXPECT_NE(bid.resting_handle, kNoHandle);
  expect_result(second_bid, RejectReason::none, 0, 5, 0, second_bid.resting_handle);
  EXPECT_NE(second_bid.resting_handle, kNoHandle);
  expect_result(ask, RejectReason::none, 0, 9, 0, ask.resting_handle);
  EXPECT_EQ(book.best_bid(), Price{103});
  EXPECT_EQ(book.best_ask(), Price{107});
  expect_level(book, Side::buy, 103, 12, 2);
  expect_level(book, Side::sell, 107, 9, 1);
  EXPECT_EQ(book.level_info(Side::buy, Price{99}), std::nullopt);
}

TEST_F(OrderBookTest, ExactFillUsesRestingMakerPriceAndRetiresLevel) {
  ASSERT_EQ(limit(10, Side::sell, 104, 6).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 106, 6);

  expect_result(result, RejectReason::none, 6, 0, 1);
  EXPECT_EQ(trades[0], (Trade{.buy_id = OrderId{20},
                              .sell_id = OrderId{10},
                              .price = Price{104},
                              .quantity = Quantity{6}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  expect_level(book, Side::sell, 104, 0, 0);
}

TEST_F(OrderBookTest, PartialMakerIsDecrementedInPlace) {
  ASSERT_EQ(limit(10, Side::sell, 104, 10).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 104, 4);

  expect_result(result, RejectReason::none, 4, 0, 1);
  EXPECT_EQ(trades[0], (Trade{.buy_id = OrderId{20},
                              .sell_id = OrderId{10},
                              .price = Price{104},
                              .quantity = Quantity{4}}));
  expect_level(book, Side::sell, 104, 6, 1);
  EXPECT_EQ(book.best_ask(), Price{104});
}

TEST_F(OrderBookTest, PartialTakerRestsResidualAtTail) {
  ASSERT_EQ(limit(10, Side::sell, 104, 3).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 104, 8);

  expect_result(result, RejectReason::none, 3, 5, 1, result.resting_handle);
  EXPECT_NE(result.resting_handle, kNoHandle);
  EXPECT_EQ(trades[0], (Trade{.buy_id = OrderId{20},
                              .sell_id = OrderId{10},
                              .price = Price{104},
                              .quantity = Quantity{3}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), Price{104});
  expect_level(book, Side::buy, 104, 5, 1);
}

TEST_F(OrderBookTest, MatchesMultipleMakersInStrictFifoOrder) {
  ASSERT_EQ(limit(11, Side::sell, 105, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::sell, 105, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(13, Side::sell, 105, 4).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 105, 6);

  expect_result(result, RejectReason::none, 6, 0, 3);
  EXPECT_EQ(trades[0], (Trade{OrderId{20}, OrderId{11}, Price{105}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{20}, OrderId{12}, Price{105}, Quantity{3}}));
  EXPECT_EQ(trades[2], (Trade{OrderId{20}, OrderId{13}, Price{105}, Quantity{1}}));
  expect_level(book, Side::sell, 105, 3, 1);
}

TEST_F(OrderBookTest, AggressiveBuyWalksAsksBestToWorseAtEachMakerPrice) {
  ASSERT_EQ(limit(11, Side::sell, 106, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::sell, 104, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(13, Side::sell, 105, 4).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 106, 8);

  expect_result(result, RejectReason::none, 8, 0, 3);
  EXPECT_EQ(trades[0], (Trade{OrderId{20}, OrderId{12}, Price{104}, Quantity{3}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{20}, OrderId{13}, Price{105}, Quantity{4}}));
  EXPECT_EQ(trades[2], (Trade{OrderId{20}, OrderId{11}, Price{106}, Quantity{1}}));
  expect_level(book, Side::sell, 106, 1, 1);
}

TEST_F(OrderBookTest, AggressiveSellWalksBidsBestToWorseAtEachMakerPrice) {
  ASSERT_EQ(limit(11, Side::buy, 104, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::buy, 106, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(13, Side::buy, 105, 4).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::sell, 104, 8);

  expect_result(result, RejectReason::none, 8, 0, 3);
  EXPECT_EQ(trades[0], (Trade{OrderId{12}, OrderId{20}, Price{106}, Quantity{3}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{13}, OrderId{20}, Price{105}, Quantity{4}}));
  EXPECT_EQ(trades[2], (Trade{OrderId{11}, OrderId{20}, Price{104}, Quantity{1}}));
  expect_level(book, Side::buy, 104, 1, 1);
}

TEST_F(OrderBookTest, NonCrossingLimitLeavesBookUncrossed) {
  ASSERT_EQ(limit(1, Side::buy, 104, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(2, Side::sell, 105, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(3, Side::buy, 105, 1).reject_reason, RejectReason::none);

  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (!bid.has_value() || !ask.has_value()) {
    FAIL() << "expected both sides of the book";
    return;
  }
  EXPECT_LT(bid.value(), ask.value());
}

TEST_F(OrderBookTest, MarketOrderSweepsAndCancelsResidual) {
  ASSERT_EQ(limit(11, Side::sell, 104, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::sell, 106, 3).reject_reason, RejectReason::none);

  const SubmitResult result = market(20, Side::buy, 8);

  expect_result(result, RejectReason::none, 5, 3, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{20}, OrderId{11}, Price{104}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{20}, OrderId{12}, Price{106}, Quantity{3}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(OrderBookTest, RejectsInvalidInputsBeforeMutation) {
  ASSERT_EQ(limit(1, Side::buy, 103, 4).reject_reason, RejectReason::none);

  expect_result(limit(2, Side::sell, 103, 0), RejectReason::zero_quantity, 0, 0, 0);
  expect_result(limit(3, Side::sell, 103, 101), RejectReason::quantity_too_large, 0, 101, 0);
  expect_result(limit(4, Side::sell, 99, 1), RejectReason::price_out_of_domain, 0, 1, 0);
  expect_result(market(5, Side::sell, 0), RejectReason::zero_quantity, 0, 0, 0);
  expect_result(market(6, Side::sell, 101), RejectReason::quantity_too_large, 0, 101, 0);
  EXPECT_EQ(book.best_bid(), Price{103});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  expect_level(book, Side::buy, 103, 4, 1);
}

TEST_F(OrderBookTest, RejectsInvalidSideBeforeLimitMutation) {
  ASSERT_EQ(limit(1, Side::sell, 103, 4).reject_reason, RejectReason::none);
  const Side invalid_side =
      static_cast<Side>(2U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)

  const SubmitResult result = limit(2, invalid_side, 103, 2);

  expect_result(result, RejectReason::invalid_side, 0, 2, 0);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), Price{103});
  expect_level(book, Side::sell, 103, 4, 1);
  EXPECT_EQ(book.level_info(invalid_side, Price{103}), std::nullopt);
}

TEST_F(OrderBookTest, RejectsInvalidSideBeforeMarketMutation) {
  ASSERT_EQ(limit(1, Side::sell, 103, 4).reject_reason, RejectReason::none);
  const Side invalid_side =
      static_cast<Side>(255U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)

  const SubmitResult result = market(2, invalid_side, 2);

  expect_result(result, RejectReason::invalid_side, 0, 2, 0);
  EXPECT_EQ(book.best_ask(), Price{103});
  expect_level(book, Side::sell, 103, 4, 1);
}

TEST_F(OrderBookTest, UndersizedOutputRejectsWithoutMutation) {
  ASSERT_EQ(limit(1, Side::sell, 103, 4).reject_reason, RejectReason::none);
  std::array<Trade, 7> too_small{};

  const SubmitResult limit_result =
      book.submit_limit(OrderId{2}, Side::buy, Price{103}, Quantity{2}, too_small);
  const SubmitResult market_result =
      book.submit_market(OrderId{3}, Side::buy, Quantity{2}, too_small);

  expect_result(limit_result, RejectReason::insufficient_trade_capacity, 0, 2, 0);
  expect_result(market_result, RejectReason::insufficient_trade_capacity, 0, 2, 0);
  expect_level(book, Side::sell, 103, 4, 1);
  EXPECT_EQ(book.required_trade_capacity(), 8U);
}

TEST(OrderBookCapacityTest, ZeroCapacityRejectsRestButAcceptsEmptyMarket) {
  OrderBook book{PriceDomain{Price{1}, 2U}, 0U, Quantity{10}};
  std::span<Trade> no_trades;

  const SubmitResult limit =
      book.submit_limit(OrderId{1}, Side::buy, Price{1}, Quantity{2}, no_trades);
  const SubmitResult market = book.submit_market(OrderId{2}, Side::buy, Quantity{2}, no_trades);

  OrderBookTest::expect_result(limit, RejectReason::order_capacity_exhausted, 0, 2, 0);
  OrderBookTest::expect_result(market, RejectReason::none, 0, 2, 0);
}

TEST(OrderBookCapacityTest, FullArenaRejectsNonCrossingOrderWithoutMutation) {
  OrderBook book{PriceDomain{Price{1}, 4U}, 1U, Quantity{10}};
  std::array<Trade, 1> trades{};
  ASSERT_EQ(book.submit_limit(OrderId{1}, Side::buy, Price{1}, Quantity{2}, trades).reject_reason,
            RejectReason::none);

  const SubmitResult result =
      book.submit_limit(OrderId{2}, Side::buy, Price{2}, Quantity{2}, trades);

  OrderBookTest::expect_result(result, RejectReason::order_capacity_exhausted, 0, 2, 0);
  EXPECT_EQ(book.best_bid(), Price{1});
}

TEST(OrderBookCapacityTest, FullArenaIncomingOrderThatFullyCrossesSucceeds) {
  OrderBook book{PriceDomain{Price{1}, 4U}, 1U, Quantity{10}};
  std::array<Trade, 1> trades{};
  ASSERT_EQ(book.submit_limit(OrderId{1}, Side::sell, Price{2}, Quantity{2}, trades).reject_reason,
            RejectReason::none);

  const SubmitResult result =
      book.submit_limit(OrderId{2}, Side::buy, Price{2}, Quantity{2}, trades);

  OrderBookTest::expect_result(result, RejectReason::none, 2, 0, 1);
  EXPECT_EQ(trades[0], (Trade{OrderId{2}, OrderId{1}, Price{2}, Quantity{2}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBookCapacityTest, FullArenaCanReuseExhaustedMakerForResidual) {
  OrderBook book{PriceDomain{Price{1}, 4U}, 1U, Quantity{10}};
  std::array<Trade, 1> trades{};
  const SubmitResult maker =
      book.submit_limit(OrderId{1}, Side::sell, Price{2}, Quantity{2}, trades);
  ASSERT_EQ(maker.reject_reason, RejectReason::none);

  const SubmitResult result =
      book.submit_limit(OrderId{2}, Side::buy, Price{2}, Quantity{5}, trades);

  OrderBookTest::expect_result(result, RejectReason::none, 2, 3, 1, result.resting_handle);
  EXPECT_EQ(trades[0], (Trade{OrderId{2}, OrderId{1}, Price{2}, Quantity{2}}));
  EXPECT_EQ(result.resting_handle.index, maker.resting_handle.index);
  EXPECT_EQ(result.resting_handle.generation,
            detail::next_generation(maker.resting_handle.generation));
  EXPECT_NE(result.resting_handle, maker.resting_handle);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), Price{2});
  OrderBookTest::expect_level(book, Side::buy, 2, 3, 1);
}

TEST(OrderBookQuantityTest, AcceptsExactUint32MaximumWhenConfigured) {
  constexpr std::uint64_t maximum = std::numeric_limits<std::uint32_t>::max();
  OrderBook book{PriceDomain{Price{7}, 1U}, 1U, Quantity{maximum}};
  std::array<Trade, 1> trades{};

  const SubmitResult result =
      book.submit_limit(OrderId{1}, Side::buy, Price{7}, Quantity{maximum}, trades);

  OrderBookTest::expect_result(result, RejectReason::none, 0, maximum, 0, result.resting_handle);
  EXPECT_NE(result.resting_handle, kNoHandle);
  OrderBookTest::expect_level(book, Side::buy, 7, maximum, 1);
}

TEST_F(OrderBookTest, DeterministicMixedOperationsPreservePriorityAndAggregates) {
  ASSERT_EQ(limit(1, Side::buy, 102, 5).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(2, Side::buy, 104, 4).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(3, Side::sell, 108, 6).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(4, Side::sell, 106, 3).reject_reason, RejectReason::none);

  const SubmitResult sell = limit(5, Side::sell, 103, 6);
  expect_result(sell, RejectReason::none, 4, 2, 1, sell.resting_handle);
  EXPECT_EQ(trades[0], (Trade{OrderId{2}, OrderId{5}, Price{104}, Quantity{4}}));
  EXPECT_EQ(book.best_bid(), Price{102});
  EXPECT_EQ(book.best_ask(), Price{103});

  const SubmitResult market_buy = market(6, Side::buy, 4);
  expect_result(market_buy, RejectReason::none, 4, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{6}, OrderId{5}, Price{103}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{6}, OrderId{4}, Price{106}, Quantity{2}}));
  expect_level(book, Side::sell, 106, 1, 1);
  expect_level(book, Side::sell, 108, 6, 1);
  EXPECT_EQ(book.best_bid(), Price{102});
  EXPECT_EQ(book.best_ask(), Price{106});
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (!bid.has_value() || !ask.has_value()) {
    FAIL() << "expected both sides of the book";
    return;
  }
  EXPECT_LT(bid.value(), ask.value());
}

} // namespace
} // namespace matching_engine
