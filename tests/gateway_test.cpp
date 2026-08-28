#include "matching_engine/gateway.hpp"

#include <gtest/gtest.h>

namespace matching_engine {

TEST(GatewayTest, RejectsDuplicateOrderIdsAndUnsafeRisk) {
  GatewayConfig config{};
  config.max_active_orders = 8U;
  config.max_quantity = Quantity{250U};
  config.max_notional = 10'000ULL;
  config.min_price = Price{-5000};
  config.max_price = Price{5000};
  config.max_orders_per_second = 3U;

  GatewayValidator validator{config};

  EXPECT_EQ(validator.validate(OrderId{1U}, Side::buy, Price{100}, Quantity{50U}, 1U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{2U}, Side::buy, Price{100}, Quantity{50U}, 2U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{2U}, Side::buy, Price{100}, Quantity{50U}, 3U),
            GatewayRejectReason::duplicate_order_id);

  EXPECT_EQ(validator.validate(OrderId{3U}, Side::buy, Price{100}, Quantity{300U}, 4U),
            GatewayRejectReason::quantity_too_large);
  EXPECT_EQ(validator.validate(OrderId{4U}, Side::buy, Price{1000}, Quantity{10U}, 5U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{5U}, Side::buy, Price{1000}, Quantity{10U}, 1005U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{6U}, Side::buy, Price{1000}, Quantity{10U}, 1005U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{7U}, Side::buy, Price{1000}, Quantity{10U}, 1005U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{8U}, Side::buy, Price{1000}, Quantity{10U}, 1005U),
            GatewayRejectReason::rate_limited);

  EXPECT_EQ(validator.validate(OrderId{9U}, Side::buy, Price{10'000}, Quantity{10U}, 9U),
            GatewayRejectReason::price_out_of_collar);

  EXPECT_EQ(validator.validate(OrderId{10U}, Side::buy, Price{1000}, Quantity{11U}, 10U),
            GatewayRejectReason::notional_too_large);
}

TEST(GatewayTest, TracksActiveOrderSetAndAcceptsReusedOrderAfterRelease) {
  GatewayConfig config{};
  config.max_active_orders = 4U;
  config.max_quantity = Quantity{100U};
  config.max_notional = 10'000ULL;
  config.min_price = Price{0};
  config.max_price = Price{10'000};
  config.max_orders_per_second = 10U;

  GatewayValidator validator{config};

  EXPECT_EQ(validator.validate(OrderId{11U}, Side::sell, Price{200}, Quantity{25U}, 11U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{12U}, Side::sell, Price{200}, Quantity{25U}, 12U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{13U}, Side::sell, Price{200}, Quantity{25U}, 13U),
            GatewayRejectReason::none);

  EXPECT_EQ(validator.validate(OrderId{14U}, Side::sell, Price{200}, Quantity{25U}, 14U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(OrderId{15U}, Side::sell, Price{200}, Quantity{25U}, 15U),
            GatewayRejectReason::order_capacity_exhausted);

  validator.release(OrderId{12U});
  EXPECT_EQ(validator.validate(OrderId{16U}, Side::sell, Price{200}, Quantity{25U}, 15U),
            GatewayRejectReason::none);
}

TEST(GatewayTest, EnforcesRateLimitsIndependentlyPerLane) {
  GatewayConfig config{};
  config.max_active_orders = 8U;
  config.max_lanes = 2U;
  config.max_quantity = Quantity{10U};
  config.max_notional = 10'000U;
  config.min_price = Price{0};
  config.max_price = Price{1'000};
  config.max_orders_per_second = 1U;
  GatewayValidator validator{config};

  EXPECT_EQ(validator.validate(LaneId{0U}, OrderId{1U}, Side::buy, Price{100}, Quantity{1U}, 1U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(LaneId{1U}, OrderId{2U}, Side::buy, Price{100}, Quantity{1U}, 1U),
            GatewayRejectReason::none);
  EXPECT_EQ(validator.validate(LaneId{0U}, OrderId{3U}, Side::buy, Price{100}, Quantity{1U}, 1U),
            GatewayRejectReason::rate_limited);
  EXPECT_EQ(validator.validate(LaneId{2U}, OrderId{4U}, Side::buy, Price{100}, Quantity{1U}, 1U),
            GatewayRejectReason::invalid_lane);
}

} // namespace matching_engine