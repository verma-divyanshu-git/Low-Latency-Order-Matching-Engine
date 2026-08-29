#include "support/reference_order_book.hpp"

#include <gtest/gtest.h>

namespace matching_engine::test {
namespace {

TEST(ReferenceOrderBookTest, MatchesAtMakerPriceAndMaintainsIndependentTokens) {
  ReferenceOrderBook book{Price{0}, 101U, 4U, Quantity{50U}};

  const ModelSubmitResult maker =
      book.submit_limit(OrderId{1}, Side::sell, Price{40}, Quantity{5}, TimeInForce::gtc, 4U);
  ASSERT_EQ(maker.reject_reason, RejectReason::none);
  ASSERT_TRUE(maker.resting_token.has_value());
  const ModelToken maker_token = maker.resting_token.value_or(0U);

  const ModelSubmitResult taker =
      book.submit_limit(OrderId{2}, Side::buy, Price{41}, Quantity{3}, TimeInForce::gtc, 4U);

  ASSERT_EQ(taker.reject_reason, RejectReason::none);
  ASSERT_EQ(taker.trades.size(), 1U);
  EXPECT_EQ(taker.trades[0], (Trade{OrderId{2}, OrderId{1}, Price{40}, Quantity{3}}));
  EXPECT_EQ(book.order_info(maker_token),
            (OrderInfo{OrderId{1}, Side::sell, Price{40}, Quantity{2}}));
  EXPECT_TRUE(taker.phantom_fills_valid);
}

TEST(ReferenceOrderBookTest, ModelsCancelAmendReplaceAndCapacityRejections) {
  ReferenceOrderBook book{Price{0}, 101U, 1U, Quantity{50U}};
  const ModelSubmitResult initial =
      book.submit_limit(OrderId{1}, Side::buy, Price{40}, Quantity{5}, TimeInForce::gtc, 1U);
  ASSERT_TRUE(initial.resting_token.has_value());
  const ModelToken initial_token = initial.resting_token.value_or(0U);

  EXPECT_EQ(book.submit_limit(OrderId{2}, Side::buy, Price{39}, Quantity{1}, TimeInForce::gtc, 1U)
                .reject_reason,
            RejectReason::order_capacity_exhausted);
  const ModelAmendResult amended = book.amend_quantity(initial_token, Quantity{3});
  EXPECT_EQ(amended.reject_reason, AmendReason::none);
  EXPECT_EQ(amended.token, initial_token);
  const ModelAmendResult equal = book.amend_quantity(initial_token, Quantity{3});
  EXPECT_EQ(equal.reject_reason, AmendReason::none);
  EXPECT_EQ(equal.token, initial_token);
  const ModelAmendResult rejected = book.amend_quantity(initial_token, Quantity{0});
  EXPECT_EQ(rejected.reject_reason, AmendReason::zero_quantity);
  EXPECT_EQ(rejected.token, initial_token);
  const ModelAmendResult invalid = book.amend_quantity(ModelToken{0U}, Quantity{3});
  EXPECT_EQ(invalid.reject_reason, AmendReason::invalid_handle);
  EXPECT_EQ(invalid.token, ModelToken{0U});
  const ModelSubmitResult replacement = book.replace(initial_token, Price{41}, Quantity{4}, 1U);
  ASSERT_EQ(replacement.reject_reason, RejectReason::none);
  ASSERT_TRUE(replacement.resting_token.has_value());
  const ModelToken replacement_token = replacement.resting_token.value_or(0U);
  EXPECT_EQ(book.order_info(initial_token), std::nullopt);
  EXPECT_EQ(book.cancel(replacement_token).reject_reason, CancelReason::none);
  EXPECT_EQ(book.live_order_count(), 0U);
}

TEST(ReferenceOrderBookTest, RejectsCrossingPostOnlyWithoutMutatingBook) {
  ReferenceOrderBook book{Price{0}, 101U, 4U, Quantity{50U}};
  ASSERT_EQ(book.submit_limit(OrderId{1U}, Side::sell, Price{40}, Quantity{5U},
                              TimeInForce::gtc, 4U)
                .reject_reason,
            RejectReason::none);

  const ModelSubmitResult rejected =
      book.submit_post_only(OrderId{2U}, Side::buy, Price{40}, Quantity{3U}, 4U);

  EXPECT_EQ(rejected.reject_reason, RejectReason::post_only_would_cross);
  EXPECT_EQ(rejected.executed_quantity, Quantity{0U});
  EXPECT_EQ(rejected.unfilled_quantity, Quantity{3U});
  EXPECT_TRUE(rejected.trades.empty());
  EXPECT_FALSE(rejected.resting_token.has_value());
  EXPECT_EQ(book.live_order_count(), 1U);
  EXPECT_EQ(book.best_ask(), Price{40});
}

TEST(ReferenceOrderBookTest, RestsNoncrossingPostOnlyOrder) {
  ReferenceOrderBook book{Price{0}, 101U, 4U, Quantity{50U}};

  const ModelSubmitResult accepted =
      book.submit_post_only(OrderId{1U}, Side::buy, Price{40}, Quantity{3U}, 4U);

  EXPECT_EQ(accepted.reject_reason, RejectReason::none);
  EXPECT_EQ(accepted.executed_quantity, Quantity{0U});
  EXPECT_EQ(accepted.unfilled_quantity, Quantity{3U});
  ASSERT_TRUE(accepted.resting_token.has_value());
  EXPECT_EQ(book.order_info(*accepted.resting_token),
            (OrderInfo{OrderId{1U}, Side::buy, Price{40}, Quantity{3U}}));
}

TEST(ReferenceOrderBookTest, CancelTakerPreventsSelfTradeByTraderIdentity) {
  ReferenceOrderBook book{Price{0}, 101U, 4U, Quantity{50U}, SelfTradePolicy::cancel_taker};
  const ModelSubmitResult maker = book.submit_limit(OrderId{1U}, TraderId{7U}, Side::sell,
                                                    Price{40}, Quantity{5U}, TimeInForce::gtc, 4U);
  ASSERT_EQ(maker.reject_reason, RejectReason::none);
  ASSERT_TRUE(maker.resting_token.has_value());

  const ModelSubmitResult blocked = book.submit_limit(OrderId{2U}, TraderId{7U}, Side::buy,
                                                      Price{40}, Quantity{3U}, TimeInForce::gtc, 4U);

  EXPECT_EQ(blocked.reject_reason, RejectReason::self_trade_prevented);
  EXPECT_EQ(blocked.executed_quantity, Quantity{0U});
  EXPECT_EQ(blocked.unfilled_quantity, Quantity{3U});
  EXPECT_TRUE(blocked.trades.empty());
  EXPECT_EQ(book.order_info(*maker.resting_token),
            (OrderInfo{OrderId{1U}, Side::sell, Price{40}, Quantity{5U}}));
}

TEST(ReferenceOrderBookTest, AllowsDifferentTradersUnderCancelTakerPolicy) {
  ReferenceOrderBook book{Price{0}, 101U, 4U, Quantity{50U}, SelfTradePolicy::cancel_taker};
  ASSERT_EQ(book.submit_limit(OrderId{1U}, TraderId{7U}, Side::sell, Price{40}, Quantity{5U},
                              TimeInForce::gtc, 4U)
                .reject_reason,
            RejectReason::none);

  const ModelSubmitResult matched = book.submit_limit(OrderId{2U}, TraderId{8U}, Side::buy,
                                                      Price{40}, Quantity{3U}, TimeInForce::gtc, 4U);

  EXPECT_EQ(matched.reject_reason, RejectReason::none);
  ASSERT_EQ(matched.trades.size(), 1U);
  EXPECT_EQ(matched.trades.front(), (Trade{OrderId{2U}, OrderId{1U}, Price{40}, Quantity{3U}}));
}

  TEST(ReferenceOrderBookTest, StopsUseLastTradeAndTriggerDeterministicCascades) {
    ReferenceOrderBook book{Price{0}, 201U, 8U, Quantity{50U}};
    ASSERT_EQ(book.submit_limit(OrderId{1U}, Side::sell, Price{100}, Quantity{2U},
                  TimeInForce::gtc, 8U)
          .reject_reason,
        RejectReason::none);
    ASSERT_EQ(book.submit_limit(OrderId{2U}, Side::sell, Price{110}, Quantity{2U},
                  TimeInForce::gtc, 8U)
          .reject_reason,
        RejectReason::none);
    ASSERT_EQ(book.submit_limit(OrderId{7U}, Side::buy, Price{99}, Quantity{1U},
                                TimeInForce::gtc, 8U)
                  .reject_reason,
              RejectReason::none);

    const ModelSubmitResult buy_stop =
      book.submit_stop(OrderId{3U}, Side::buy, Price{100}, Quantity{1U}, 8U);
    ASSERT_TRUE(buy_stop.resting_token.has_value());
    const ModelSubmitResult sell_stop =
        book.submit_stop_limit(OrderId{4U}, Side::sell, Price{110}, Price{99}, Quantity{1U}, 8U);
    ASSERT_TRUE(sell_stop.resting_token.has_value());

    const ModelSubmitResult trigger =
      book.submit_market(OrderId{5U}, Side::buy, Quantity{2U}, 8U);

    ASSERT_EQ(trigger.trades.size(), 3U);
    EXPECT_EQ(trigger.trades[0], (Trade{OrderId{5U}, OrderId{1U}, Price{100}, Quantity{2U}}));
    EXPECT_EQ(trigger.trades[1], (Trade{OrderId{3U}, OrderId{2U}, Price{110}, Quantity{1U}}));
    EXPECT_EQ(trigger.trades[2], (Trade{OrderId{7U}, OrderId{4U}, Price{99}, Quantity{1U}}));
    EXPECT_EQ(book.last_execution_price(), Price{99});
    EXPECT_EQ(book.cancel(*buy_stop.resting_token).reject_reason, CancelReason::invalid_handle);
    EXPECT_EQ(book.cancel(*sell_stop.resting_token).reject_reason, CancelReason::invalid_handle);

    const ModelSubmitResult immediate =
      book.submit_stop_limit(OrderId{6U}, Side::buy, Price{90}, Price{98}, Quantity{1U}, 8U);
    EXPECT_TRUE(immediate.resting_token.has_value());
    EXPECT_EQ(immediate.unfilled_quantity, Quantity{1U});
    EXPECT_EQ(book.best_bid(), Price{98});
  }

} // namespace
} // namespace matching_engine::test
