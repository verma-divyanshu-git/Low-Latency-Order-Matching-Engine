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
  EXPECT_EQ(book.amend_quantity(initial_token, Quantity{3}).reject_reason, AmendReason::none);
  const ModelSubmitResult replacement = book.replace(initial_token, Price{41}, Quantity{4}, 1U);
  ASSERT_EQ(replacement.reject_reason, RejectReason::none);
  ASSERT_TRUE(replacement.resting_token.has_value());
  const ModelToken replacement_token = replacement.resting_token.value_or(0U);
  EXPECT_EQ(book.order_info(initial_token), std::nullopt);
  EXPECT_EQ(book.cancel(replacement_token).reject_reason, CancelReason::none);
  EXPECT_EQ(book.live_order_count(), 0U);
}

} // namespace
} // namespace matching_engine::test
