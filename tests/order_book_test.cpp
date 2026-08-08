#include "matching_engine/order_book.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace matching_engine {
namespace {

static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_trivially_copyable_v<SubmitResult>);
static_assert(std::is_trivially_copyable_v<CancelResult>);
static_assert(std::is_trivially_copyable_v<AmendResult>);
static_assert(std::is_trivially_copyable_v<InvariantResult>);
static_assert(std::is_trivially_copyable_v<OrderInfo>);
static_assert(noexcept(std::declval<OrderBook&>().check_invariants()));
static_assert(noexcept(std::declval<const OrderBook&>().order_info(Handle{})));
static_assert(InvariantViolation::bitmap_hierarchy_inconsistent != InvariantViolation::none);
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

  [[nodiscard]] SubmitResult limit(std::uint64_t id, Side side, std::int64_t price,
                                   std::uint64_t quantity, TimeInForce tif) {
    return book.submit_limit(OrderId{id}, side, Price{price}, Quantity{quantity}, tif, trades);
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

  static void expect_invariants(OrderBook& target, std::uint32_t reachable_count) {
    const InvariantResult result = target.check_invariants();
    EXPECT_EQ(result.violation, InvariantViolation::none);
    EXPECT_EQ(result.side, Side::buy);
    EXPECT_EQ(result.level_index, kInvalidIndex);
    EXPECT_EQ(result.order_index, kInvalidIndex);
    EXPECT_EQ(result.reachable_count, reachable_count);
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

TEST(OrderBookConstructionTest, EnforcesExplicitPriceLevelMemoryCeiling) {
  EXPECT_NO_THROW((OrderBook{PriceDomain{Price{0}, kMaximumPriceLevels}, 0U, Quantity{1U}}));
  EXPECT_THROW((OrderBook{PriceDomain{Price{0}, kMaximumPriceLevels + 1U}, 0U, Quantity{1U}}),
               std::length_error);
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

TEST_F(OrderBookTest, OrderInfoReturnsLiveStateAndRejectsStaleAndOutOfRangeHandles) {
  const SubmitResult resting = limit(42, Side::sell, 104, 7);
  ASSERT_EQ(resting.reject_reason, RejectReason::none);

  EXPECT_EQ(book.order_info(resting.resting_handle),
            (OrderInfo{OrderId{42}, Side::sell, Price{104}, Quantity{7}}));
  EXPECT_EQ(book.order_info(Handle{kInvalidIndex, 1U}), std::nullopt);
  EXPECT_EQ(book.order_info(Handle{resting.resting_handle.index, 0U}), std::nullopt);

  ASSERT_EQ(book.amend_quantity(resting.resting_handle, Quantity{3}).reject_reason,
            AmendReason::none);
  EXPECT_EQ(book.order_info(resting.resting_handle),
            (OrderInfo{OrderId{42}, Side::sell, Price{104}, Quantity{3}}));
  ASSERT_EQ(book.cancel(resting.resting_handle).reject_reason, CancelReason::none);
  EXPECT_EQ(book.order_info(resting.resting_handle), std::nullopt);
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

TEST_F(OrderBookTest, IocReportsPartialNoFillAndFullFillWithoutRestingResidual) {
  ASSERT_EQ(limit(10, Side::sell, 104, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(11, Side::sell, 105, 2).reject_reason, RejectReason::none);

  const SubmitResult partial = limit(20, Side::buy, 104, 5, TimeInForce::ioc);
  expect_result(partial, RejectReason::none, 3, 2, 1);
  EXPECT_EQ(trades[0], (Trade{OrderId{20}, OrderId{10}, Price{104}, Quantity{3}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), Price{105});

  const SubmitResult no_fill = limit(21, Side::buy, 103, 4, TimeInForce::ioc);
  expect_result(no_fill, RejectReason::none, 0, 4, 0);
  EXPECT_EQ(book.best_bid(), std::nullopt);

  const SubmitResult full = limit(22, Side::buy, 105, 2, TimeInForce::ioc);
  expect_result(full, RejectReason::none, 2, 0, 1);
  EXPECT_EQ(trades[0], (Trade{OrderId{22}, OrderId{11}, Price{105}, Quantity{2}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(OrderBookTest, FokInsufficientAcrossLevelsLeavesBookAndTradesUnchanged) {
  const SubmitResult first = limit(10, Side::sell, 103, 2);
  const SubmitResult second = limit(11, Side::sell, 105, 3);
  ASSERT_EQ(first.reject_reason, RejectReason::none);
  ASSERT_EQ(second.reject_reason, RejectReason::none);
  trades.fill(Trade{OrderId{91}, OrderId{92}, Price{109}, Quantity{93}});
  const auto before = trades;

  const SubmitResult one_level = limit(20, Side::buy, 103, 3, TimeInForce::fok);
  expect_result(one_level, RejectReason::fok_not_fillable, 0, 3, 0);
  EXPECT_EQ(trades, before);
  expect_level(book, Side::sell, 103, 2, 1);

  const SubmitResult multiple_levels = limit(21, Side::buy, 105, 6, TimeInForce::fok);
  expect_result(multiple_levels, RejectReason::fok_not_fillable, 0, 6, 0);
  EXPECT_EQ(trades, before);
  EXPECT_EQ(book.best_ask(), Price{103});
  expect_level(book, Side::sell, 103, 2, 1);
  expect_level(book, Side::sell, 105, 3, 1);
  EXPECT_EQ(book.cancel(first.resting_handle).reject_reason, CancelReason::none);
  EXPECT_EQ(book.cancel(second.resting_handle).reject_reason, CancelReason::none);
}

TEST_F(OrderBookTest, FokExactFillAcrossLevelsUsesPriceThenFifoPriority) {
  ASSERT_EQ(limit(10, Side::sell, 103, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(11, Side::sell, 104, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::sell, 104, 4).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::buy, 104, 9, TimeInForce::fok);

  expect_result(result, RejectReason::none, 9, 0, 3);
  EXPECT_EQ(trades[0], (Trade{OrderId{20}, OrderId{10}, Price{103}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{20}, OrderId{11}, Price{104}, Quantity{3}}));
  EXPECT_EQ(trades[2], (Trade{OrderId{20}, OrderId{12}, Price{104}, Quantity{4}}));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(OrderBookTest, SellFokTraversesBidsDescendingAndIncludesLimitPrice) {
  ASSERT_EQ(limit(10, Side::buy, 110, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(11, Side::buy, 108, 3).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(12, Side::buy, 106, 4).reject_reason, RejectReason::none);

  const SubmitResult result = limit(20, Side::sell, 108, 5, TimeInForce::fok);

  expect_result(result, RejectReason::none, 5, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{10}, OrderId{20}, Price{110}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{11}, OrderId{20}, Price{108}, Quantity{3}}));
  EXPECT_EQ(book.best_bid(), Price{106});
  expect_level(book, Side::buy, 106, 4, 1);
}

TEST_F(OrderBookTest, SellFokInsufficientAtDomainEdgeLeavesBookAndTradesUnchanged) {
  const SubmitResult edge = limit(10, Side::buy, 110, 2);
  const SubmitResult below_limit = limit(11, Side::buy, 108, 3);
  ASSERT_EQ(edge.reject_reason, RejectReason::none);
  ASSERT_EQ(below_limit.reject_reason, RejectReason::none);
  trades.fill(Trade{OrderId{91}, OrderId{92}, Price{109}, Quantity{93}});
  const auto before = trades;

  const SubmitResult result = limit(20, Side::sell, 109, 3, TimeInForce::fok);

  expect_result(result, RejectReason::fok_not_fillable, 0, 3, 0);
  EXPECT_EQ(trades, before);
  EXPECT_EQ(book.best_bid(), Price{110});
  expect_level(book, Side::buy, 110, 2, 1);
  expect_level(book, Side::buy, 108, 3, 1);
  EXPECT_EQ(book.cancel(edge.resting_handle),
            (CancelResult{CancelReason::none, OrderId{10}, Quantity{2}}));
  EXPECT_EQ(book.cancel(below_limit.resting_handle),
            (CancelResult{CancelReason::none, OrderId{11}, Quantity{3}}));
}

TEST(OrderBookFokEdgeTest, SellFokAcceptsMinimumDomainLimit) {
  OrderBook book{PriceDomain{Price{100}, 11U}, 2U, Quantity{10U}};
  std::array<Trade, 2> trades{};
  ASSERT_EQ(book.submit_limit(OrderId{1}, Side::buy, Price{101}, Quantity{1}, trades).reject_reason,
            RejectReason::none);
  ASSERT_EQ(book.submit_limit(OrderId{2}, Side::buy, Price{100}, Quantity{1}, trades).reject_reason,
            RejectReason::none);

  const SubmitResult result =
      book.submit_limit(OrderId{3}, Side::sell, Price{100}, Quantity{2}, TimeInForce::fok, trades);

  OrderBookTest::expect_result(result, RejectReason::none, 2, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{1}, OrderId{3}, Price{101}, Quantity{1}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{2}, OrderId{3}, Price{100}, Quantity{1}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(OrderBookTest, InvalidTimeInForceRejectsBeforeMutation) {
  const SubmitResult maker = limit(10, Side::sell, 104, 4);
  ASSERT_EQ(maker.reject_reason, RejectReason::none);
  trades.fill(Trade{OrderId{91}, OrderId{92}, Price{109}, Quantity{93}});
  const auto before = trades;
  const TimeInForce invalid =
      static_cast<TimeInForce>(255U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)

  const SubmitResult result = limit(20, Side::buy, 104, 2, invalid);

  expect_result(result, RejectReason::invalid_time_in_force, 0, 2, 0);
  EXPECT_EQ(trades, before);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), Price{104});
  expect_level(book, Side::sell, 104, 4, 1);
  EXPECT_EQ(book.cancel(maker.resting_handle).reject_reason, CancelReason::none);
}

TEST(OrderBookCapacityTest, FullArenaDoesNotBlockIocOrFok) {
  OrderBook book{PriceDomain{Price{1}, 4U}, 1U, Quantity{10}};
  std::array<Trade, 1> trades{};
  ASSERT_EQ(book.submit_limit(OrderId{1}, Side::sell, Price{2}, Quantity{4}, trades).reject_reason,
            RejectReason::none);

  const SubmitResult ioc =
      book.submit_limit(OrderId{2}, Side::buy, Price{1}, Quantity{2}, TimeInForce::ioc, trades);
  OrderBookTest::expect_result(ioc, RejectReason::none, 0, 2, 0);

  const SubmitResult fok =
      book.submit_limit(OrderId{3}, Side::buy, Price{2}, Quantity{4}, TimeInForce::fok, trades);
  OrderBookTest::expect_result(fok, RejectReason::none, 4, 0, 1);
  EXPECT_EQ(trades[0], (Trade{OrderId{3}, OrderId{1}, Price{2}, Quantity{4}}));
}

TEST_F(OrderBookTest, CancelUnlinksOnlyHeadMiddleAndTailAndUpdatesAggregates) {
  const SubmitResult first = limit(1, Side::buy, 104, 1);
  const SubmitResult middle = limit(2, Side::buy, 104, 2);
  const SubmitResult third = limit(3, Side::buy, 104, 3);
  const SubmitResult tail = limit(4, Side::buy, 104, 4);
  ASSERT_EQ(first.reject_reason, RejectReason::none);
  ASSERT_EQ(middle.reject_reason, RejectReason::none);
  ASSERT_EQ(third.reject_reason, RejectReason::none);
  ASSERT_EQ(tail.reject_reason, RejectReason::none);

  const CancelResult tail_cancel = book.cancel(tail.resting_handle);
  EXPECT_EQ(tail_cancel, (CancelResult{CancelReason::none, OrderId{4}, Quantity{4}}));
  expect_level(book, Side::buy, 104, 6, 3);

  const CancelResult middle_cancel = book.cancel(middle.resting_handle);
  EXPECT_EQ(middle_cancel, (CancelResult{CancelReason::none, OrderId{2}, Quantity{2}}));
  expect_level(book, Side::buy, 104, 4, 2);

  const CancelResult head_cancel = book.cancel(first.resting_handle);
  EXPECT_EQ(head_cancel, (CancelResult{CancelReason::none, OrderId{1}, Quantity{1}}));
  expect_level(book, Side::buy, 104, 3, 1);

  const CancelResult only_cancel = book.cancel(third.resting_handle);
  EXPECT_EQ(only_cancel, (CancelResult{CancelReason::none, OrderId{3}, Quantity{3}}));
  expect_level(book, Side::buy, 104, 0, 0);
  EXPECT_EQ(book.best_bid(), std::nullopt);

  const SubmitResult ask = limit(5, Side::sell, 106, 2);
  ASSERT_EQ(ask.reject_reason, RejectReason::none);
  EXPECT_EQ(book.cancel(ask.resting_handle).reject_reason, CancelReason::none);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookCancelLinksTest, CancelHeadPreservesSuccessorFifoChain) {
  OrderBook book{PriceDomain{Price{100}, 11U}, 4U, Quantity{100U}};
  std::array<Trade, 4> trades{};
  const SubmitResult head =
      book.submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{1}, trades);
  ASSERT_EQ(head.reject_reason, RejectReason::none);
  ASSERT_EQ(
      book.submit_limit(OrderId{2}, Side::sell, Price{104}, Quantity{2}, trades).reject_reason,
      RejectReason::none);
  ASSERT_EQ(
      book.submit_limit(OrderId{3}, Side::sell, Price{104}, Quantity{3}, trades).reject_reason,
      RejectReason::none);
  ASSERT_EQ(book.cancel(head.resting_handle).reject_reason, CancelReason::none);

  const SubmitResult fill =
      book.submit_limit(OrderId{9}, Side::buy, Price{104}, Quantity{5}, trades);

  OrderBookTest::expect_result(fill, RejectReason::none, 5, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{9}, OrderId{2}, Price{104}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{9}, OrderId{3}, Price{104}, Quantity{3}}));
  OrderBookTest::expect_level(book, Side::sell, 104, 0, 0);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookCancelLinksTest, CancelMiddlePreservesPredecessorSuccessorFifoChain) {
  OrderBook book{PriceDomain{Price{100}, 11U}, 4U, Quantity{100U}};
  std::array<Trade, 4> trades{};
  ASSERT_EQ(
      book.submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{1}, trades).reject_reason,
      RejectReason::none);
  const SubmitResult middle =
      book.submit_limit(OrderId{2}, Side::sell, Price{104}, Quantity{2}, trades);
  ASSERT_EQ(middle.reject_reason, RejectReason::none);
  ASSERT_EQ(
      book.submit_limit(OrderId{3}, Side::sell, Price{104}, Quantity{3}, trades).reject_reason,
      RejectReason::none);
  ASSERT_EQ(book.cancel(middle.resting_handle).reject_reason, CancelReason::none);

  const SubmitResult fill =
      book.submit_limit(OrderId{9}, Side::buy, Price{104}, Quantity{4}, trades);

  OrderBookTest::expect_result(fill, RejectReason::none, 4, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{9}, OrderId{1}, Price{104}, Quantity{1}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{9}, OrderId{3}, Price{104}, Quantity{3}}));
  OrderBookTest::expect_level(book, Side::sell, 104, 0, 0);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookCancelLinksTest, CancelTailPreservesPredecessorFifoChain) {
  OrderBook book{PriceDomain{Price{100}, 11U}, 4U, Quantity{100U}};
  std::array<Trade, 4> trades{};
  ASSERT_EQ(
      book.submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{1}, trades).reject_reason,
      RejectReason::none);
  ASSERT_EQ(
      book.submit_limit(OrderId{2}, Side::sell, Price{104}, Quantity{2}, trades).reject_reason,
      RejectReason::none);
  const SubmitResult tail =
      book.submit_limit(OrderId{3}, Side::sell, Price{104}, Quantity{3}, trades);
  ASSERT_EQ(tail.reject_reason, RejectReason::none);
  ASSERT_EQ(book.cancel(tail.resting_handle).reject_reason, CancelReason::none);

  const SubmitResult fill =
      book.submit_limit(OrderId{9}, Side::buy, Price{104}, Quantity{3}, trades);

  OrderBookTest::expect_result(fill, RejectReason::none, 3, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{9}, OrderId{1}, Price{104}, Quantity{1}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{9}, OrderId{2}, Price{104}, Quantity{2}}));
  OrderBookTest::expect_level(book, Side::sell, 104, 0, 0);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(OrderBookTest, CancelRejectsInvalidHandlesAndReusesFreedGeneration) {
  const SubmitResult order = limit(1, Side::buy, 104, 5);
  ASSERT_EQ(order.reject_reason, RejectReason::none);
  EXPECT_EQ(book.cancel(order.resting_handle).reject_reason, CancelReason::none);
  const auto unchanged = book.level_info(Side::buy, Price{104});

  EXPECT_EQ(book.cancel(order.resting_handle),
            (CancelResult{CancelReason::invalid_handle, OrderId{0}, Quantity{0}}));
  EXPECT_EQ(book.cancel(Handle{order.resting_handle.index, 0U}),
            (CancelResult{CancelReason::invalid_handle, OrderId{0}, Quantity{0}}));
  EXPECT_EQ(book.cancel(Handle{kInvalidIndex, 1U}),
            (CancelResult{CancelReason::invalid_handle, OrderId{0}, Quantity{0}}));
  EXPECT_EQ(book.level_info(Side::buy, Price{104}), unchanged);

  const SubmitResult reused = limit(2, Side::buy, 104, 6);
  EXPECT_EQ(reused.resting_handle.index, order.resting_handle.index);
  EXPECT_EQ(reused.resting_handle.generation,
            detail::next_generation(order.resting_handle.generation));
}

TEST_F(OrderBookTest, AmendDecreaseRetainsHandlePositionAndEqualIsNoOp) {
  const SubmitResult first = limit(1, Side::sell, 104, 5);
  const SubmitResult second = limit(2, Side::sell, 104, 4);
  ASSERT_EQ(first.reject_reason, RejectReason::none);
  ASSERT_EQ(second.reject_reason, RejectReason::none);

  const AmendResult decreased = book.amend_quantity(first.resting_handle, Quantity{3});
  EXPECT_EQ(decreased, (AmendResult{AmendReason::none, OrderId{1}, Quantity{5}, Quantity{3},
                                    first.resting_handle}));
  const AmendResult equal = book.amend_quantity(first.resting_handle, Quantity{3});
  EXPECT_EQ(equal, (AmendResult{AmendReason::none, OrderId{1}, Quantity{3}, Quantity{3},
                                first.resting_handle}));
  expect_level(book, Side::sell, 104, 7, 2);

  const SubmitResult fill = limit(9, Side::buy, 104, 4);
  EXPECT_EQ(trades[0], (Trade{OrderId{9}, OrderId{1}, Price{104}, Quantity{3}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{9}, OrderId{2}, Price{104}, Quantity{1}}));
  expect_result(fill, RejectReason::none, 4, 0, 2);
}

TEST_F(OrderBookTest, AmendRejectsZeroIncreaseMaximumAndStaleWithoutMutation) {
  const SubmitResult order = limit(1, Side::buy, 104, 5);
  ASSERT_EQ(order.reject_reason, RejectReason::none);

  EXPECT_EQ(book.amend_quantity(order.resting_handle, Quantity{0}),
            (AmendResult{AmendReason::zero_quantity, OrderId{1}, Quantity{5}, Quantity{0},
                         order.resting_handle}));
  EXPECT_EQ(book.amend_quantity(order.resting_handle, Quantity{6}),
            (AmendResult{AmendReason::increase_not_allowed, OrderId{1}, Quantity{5}, Quantity{6},
                         order.resting_handle}));
  EXPECT_EQ(book.amend_quantity(order.resting_handle, Quantity{101}),
            (AmendResult{AmendReason::quantity_too_large, OrderId{1}, Quantity{5}, Quantity{101},
                         order.resting_handle}));
  expect_level(book, Side::buy, 104, 5, 1);

  ASSERT_EQ(book.cancel(order.resting_handle).reject_reason, CancelReason::none);
  EXPECT_EQ(book.amend_quantity(order.resting_handle, Quantity{3}),
            (AmendResult{AmendReason::invalid_handle, OrderId{0}, Quantity{0}, Quantity{3},
                         order.resting_handle}));
  expect_level(book, Side::buy, 104, 0, 0);
}

TEST_F(OrderBookTest, ReplaceAtSamePriceLosesPriorityAndMakesOldHandleStale) {
  const SubmitResult first = limit(1, Side::sell, 104, 2);
  const SubmitResult second = limit(2, Side::sell, 104, 2);
  ASSERT_EQ(first.reject_reason, RejectReason::none);
  ASSERT_EQ(second.reject_reason, RejectReason::none);

  const SubmitResult replacement =
      book.replace(first.resting_handle, Price{104}, Quantity{2}, trades);

  expect_result(replacement, RejectReason::none, 0, 2, 0, replacement.resting_handle);
  EXPECT_EQ(book.cancel(first.resting_handle).reject_reason, CancelReason::invalid_handle);
  EXPECT_EQ(replacement.resting_handle.index, first.resting_handle.index);
  EXPECT_EQ(replacement.resting_handle.generation,
            detail::next_generation(first.resting_handle.generation));
  const SubmitResult fill = limit(9, Side::buy, 104, 4);
  expect_result(fill, RejectReason::none, 4, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{9}, OrderId{2}, Price{104}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{9}, OrderId{1}, Price{104}, Quantity{2}}));
}

TEST_F(OrderBookTest, ReplaceAtChangedPriceCrossesAtMakerPrices) {
  const SubmitResult original = limit(1, Side::buy, 102, 5);
  ASSERT_EQ(original.reject_reason, RejectReason::none);
  ASSERT_EQ(limit(2, Side::sell, 104, 2).reject_reason, RejectReason::none);
  ASSERT_EQ(limit(3, Side::sell, 105, 4).reject_reason, RejectReason::none);

  const SubmitResult replacement =
      book.replace(original.resting_handle, Price{105}, Quantity{5}, trades);

  expect_result(replacement, RejectReason::none, 5, 0, 2);
  EXPECT_EQ(trades[0], (Trade{OrderId{1}, OrderId{2}, Price{104}, Quantity{2}}));
  EXPECT_EQ(trades[1], (Trade{OrderId{1}, OrderId{3}, Price{105}, Quantity{3}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  expect_level(book, Side::sell, 105, 1, 1);
}

TEST_F(OrderBookTest, InvalidReplacementLeavesOriginalLiveAndUnchanged) {
  const SubmitResult original = limit(1, Side::buy, 103, 5);
  ASSERT_EQ(original.reject_reason, RejectReason::none);
  trades.fill(Trade{OrderId{91}, OrderId{92}, Price{109}, Quantity{93}});
  const auto trades_before = trades;
  std::array<Trade, 7> too_small{};
  too_small.fill(Trade{OrderId{81}, OrderId{82}, Price{108}, Quantity{83}});
  const auto too_small_before = too_small;

  expect_result(book.replace(original.resting_handle, Price{99}, Quantity{4}, trades),
                RejectReason::price_out_of_domain, 0, 4, 0);
  EXPECT_EQ(trades, trades_before);
  EXPECT_EQ(too_small, too_small_before);
  EXPECT_EQ(book.best_bid(), Price{103});
  expect_level(book, Side::buy, 103, 5, 1);

  expect_result(book.replace(original.resting_handle, Price{104}, Quantity{0}, trades),
                RejectReason::zero_quantity, 0, 0, 0);
  EXPECT_EQ(trades, trades_before);
  EXPECT_EQ(too_small, too_small_before);
  EXPECT_EQ(book.best_bid(), Price{103});
  expect_level(book, Side::buy, 103, 5, 1);

  expect_result(book.replace(original.resting_handle, Price{104}, Quantity{101}, trades),
                RejectReason::quantity_too_large, 0, 101, 0);
  EXPECT_EQ(trades, trades_before);
  EXPECT_EQ(too_small, too_small_before);
  EXPECT_EQ(book.best_bid(), Price{103});
  expect_level(book, Side::buy, 103, 5, 1);

  expect_result(book.replace(original.resting_handle, Price{104}, Quantity{4}, too_small),
                RejectReason::insufficient_trade_capacity, 0, 4, 0);
  EXPECT_EQ(trades, trades_before);
  EXPECT_EQ(too_small, too_small_before);
  EXPECT_EQ(book.best_bid(), Price{103});
  expect_level(book, Side::buy, 103, 5, 1);

  expect_result(book.replace(Handle{kInvalidIndex, 1U}, Price{104}, Quantity{4}, trades),
                RejectReason::invalid_handle, 0, 4, 0);
  EXPECT_EQ(trades, trades_before);
  EXPECT_EQ(too_small, too_small_before);
  EXPECT_EQ(book.best_bid(), Price{103});
  expect_level(book, Side::buy, 103, 5, 1);
  EXPECT_EQ(book.best_ask(), std::nullopt);

  const CancelResult cancel = book.cancel(original.resting_handle);
  EXPECT_EQ(cancel, (CancelResult{CancelReason::none, OrderId{1}, Quantity{5}}));
}

TEST(OrderBookInvariantTest, AcceptsEveryMeaningfulOrderLifecycleCategory) {
  const auto make_book = [] {
    return std::make_unique<OrderBook>(PriceDomain{Price{100}, 11U}, 8U, Quantity{100U});
  };
  std::array<Trade, 8> trades{};

  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::buy, Price{103}, Quantity{2}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::buy, Price{103}, Quantity{3}, trades).reject_reason,
        RejectReason::none);
    OrderBookTest::expect_invariants(*book, 2U);
  }
  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::sell, Price{103}, Quantity{2}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::sell, Price{104}, Quantity{3}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{3}, Side::buy, Price{104}, Quantity{4}, trades).reject_reason,
        RejectReason::none);
    OrderBookTest::expect_invariants(*book, 1U);
  }
  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{5}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::buy, Price{104}, Quantity{2}, trades).reject_reason,
        RejectReason::none);
    OrderBookTest::expect_invariants(*book, 1U);
  }
  {
    auto book = make_book();
    const SubmitResult head =
        book->submit_limit(OrderId{1}, Side::buy, Price{104}, Quantity{1}, trades);
    const SubmitResult middle =
        book->submit_limit(OrderId{2}, Side::buy, Price{104}, Quantity{2}, trades);
    const SubmitResult tail =
        book->submit_limit(OrderId{3}, Side::buy, Price{104}, Quantity{3}, trades);
    ASSERT_EQ(book->cancel(middle.resting_handle).reject_reason, CancelReason::none);
    OrderBookTest::expect_invariants(*book, 2U);
    ASSERT_EQ(book->cancel(head.resting_handle).reject_reason, CancelReason::none);
    OrderBookTest::expect_invariants(*book, 1U);
    ASSERT_EQ(book->cancel(tail.resting_handle).reject_reason, CancelReason::none);
    OrderBookTest::expect_invariants(*book, 0U);
  }
  {
    auto book = make_book();
    const SubmitResult order =
        book->submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{5}, trades);
    ASSERT_EQ(book->amend_quantity(order.resting_handle, Quantity{3}).reject_reason,
              AmendReason::none);
    OrderBookTest::expect_invariants(*book, 1U);
    ASSERT_EQ(book->replace(order.resting_handle, Price{105}, Quantity{2}, trades).reject_reason,
              RejectReason::none);
    OrderBookTest::expect_invariants(*book, 1U);
  }
  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{2}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::buy, Price{104}, Quantity{3}, TimeInForce::ioc, trades)
            .reject_reason,
        RejectReason::none);
    OrderBookTest::expect_invariants(*book, 0U);
  }
  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{2}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::buy, Price{104}, Quantity{2}, TimeInForce::fok, trades)
            .reject_reason,
        RejectReason::none);
    OrderBookTest::expect_invariants(*book, 0U);
  }
  {
    OrderBook book{PriceDomain{Price{100}, 2U}, 1U, Quantity{100U}};
    std::array<Trade, 1> one_trade{};
    const SubmitResult maker =
        book.submit_limit(OrderId{1}, Side::sell, Price{101}, Quantity{2}, one_trade);
    const SubmitResult reused =
        book.submit_limit(OrderId{2}, Side::buy, Price{101}, Quantity{3}, one_trade);
    ASSERT_EQ(reused.resting_handle.index, maker.resting_handle.index);
    OrderBookTest::expect_invariants(book, 1U);
  }
  {
    auto book = make_book();
    ASSERT_EQ(
        book->submit_limit(OrderId{1}, Side::buy, Price{102}, Quantity{5}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(
        book->submit_limit(OrderId{2}, Side::sell, Price{106}, Quantity{3}, trades).reject_reason,
        RejectReason::none);
    ASSERT_EQ(book->submit_market(OrderId{3}, Side::buy, Quantity{1}, trades).reject_reason,
              RejectReason::none);
    OrderBookTest::expect_invariants(*book, 2U);
  }
  {
    OrderBook book{PriceDomain{Price{100}, 2U}, 0U, Quantity{100U}};
    OrderBookTest::expect_invariants(book, 0U);
    OrderBookTest::expect_invariants(book, 0U);
  }
}

TEST(OrderBookReplayTest, IdenticalCommandStreamsProduceIdenticalResultsAndState) {
  OrderBook first{PriceDomain{Price{100}, 11U}, 6U, Quantity{100U}};
  OrderBook second{PriceDomain{Price{100}, 11U}, 6U, Quantity{100U}};
  std::array<Trade, 6> first_trades{};
  std::array<Trade, 6> second_trades{};

  const auto expect_submit = [&](const SubmitResult& lhs, const SubmitResult& rhs) {
    EXPECT_EQ(lhs.reject_reason, rhs.reject_reason);
    EXPECT_EQ(lhs.executed_quantity, rhs.executed_quantity);
    EXPECT_EQ(lhs.unfilled_quantity, rhs.unfilled_quantity);
    EXPECT_EQ(lhs.trade_count, rhs.trade_count);
    EXPECT_EQ(lhs.resting_handle, rhs.resting_handle);
    for (std::uint32_t index = 0U; index < lhs.trade_count; ++index) {
      EXPECT_EQ(first_trades[index], second_trades[index]);
    }
  };
  const auto expect_state = [&] {
    EXPECT_EQ(first.best_bid(), second.best_bid());
    EXPECT_EQ(first.best_ask(), second.best_ask());
    for (const Side side : {Side::buy, Side::sell}) {
      for (std::int64_t price = 100; price <= 110; ++price) {
        EXPECT_EQ(first.level_info(side, Price{price}), second.level_info(side, Price{price}));
      }
    }
    EXPECT_EQ(first.check_invariants(), second.check_invariants());
  };

  const SubmitResult first_bid =
      first.submit_limit(OrderId{1}, Side::buy, Price{103}, Quantity{5}, first_trades);
  const SubmitResult second_bid =
      second.submit_limit(OrderId{1}, Side::buy, Price{103}, Quantity{5}, second_trades);
  expect_submit(first_bid, second_bid);
  expect_state();

  const SubmitResult first_ask =
      first.submit_limit(OrderId{2}, Side::sell, Price{106}, Quantity{4}, first_trades);
  const SubmitResult second_ask =
      second.submit_limit(OrderId{2}, Side::sell, Price{106}, Quantity{4}, second_trades);
  expect_submit(first_ask, second_ask);
  expect_state();

  const AmendResult first_amend = first.amend_quantity(first_bid.resting_handle, Quantity{3});
  const AmendResult second_amend = second.amend_quantity(second_bid.resting_handle, Quantity{3});
  EXPECT_EQ(first_amend, second_amend);
  expect_state();

  const SubmitResult first_sweep =
      first.submit_limit(OrderId{3}, Side::buy, Price{107}, Quantity{6}, first_trades);
  const SubmitResult second_sweep =
      second.submit_limit(OrderId{3}, Side::buy, Price{107}, Quantity{6}, second_trades);
  expect_submit(first_sweep, second_sweep);
  expect_state();

  const CancelResult first_cancel = first.cancel(first_bid.resting_handle);
  const CancelResult second_cancel = second.cancel(second_bid.resting_handle);
  EXPECT_EQ(first_cancel, second_cancel);
  expect_state();

  const SubmitResult first_ioc = first.submit_limit(OrderId{4}, Side::sell, Price{106}, Quantity{2},
                                                    TimeInForce::ioc, first_trades);
  const SubmitResult second_ioc = second.submit_limit(OrderId{4}, Side::sell, Price{106},
                                                      Quantity{2}, TimeInForce::ioc, second_trades);
  expect_submit(first_ioc, second_ioc);
  expect_state();
}

} // namespace
} // namespace matching_engine
