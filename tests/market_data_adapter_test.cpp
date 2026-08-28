#include "matching_engine/market_data_adapter.hpp"

#include <gtest/gtest.h>

namespace matching_engine {
namespace {

GatewayConfig gateway_config() {
  return GatewayConfig{.max_active_orders = 8U,
                       .max_lanes = 1U,
                       .max_quantity = Quantity{10U},
                       .max_notional = 1'000U,
                       .min_price = Price{0},
                       .max_price = Price{200},
                       .max_orders_per_second = 8U};
}

} // namespace

TEST(MarketDataAdapterTest, AdaptsContiguousAddOrdersToEngineCommands) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  const MarketDataMessage first{.sequence = 7U,
                                .order_id = OrderId{11U},
                                .price = Price{101},
                                .quantity = Quantity{4U},
                                .type = MarketDataMessageType::add_order,
                                .side = Side::sell};
  const MarketDataMessage second{.sequence = 8U,
                                 .order_id = OrderId{12U},
                                 .price = Price{100},
                                 .quantity = Quantity{3U},
                                 .type = MarketDataMessageType::add_order,
                                 .side = Side::buy};

  const SequencedCommand expected{CommandPayload::submit_limit(OrderId{11U}, Side::sell,
                                                                 Price{101}, Quantity{4U}),
                                  Sequence{1U}, 7U};
  EXPECT_EQ(adapter.adapt(first), expected);
  EXPECT_EQ(adapter.adapt(second)->sequence, Sequence{2U});
}

TEST(MarketDataAdapterTest, RejectsUnsupportedMessagesAndSequenceGaps) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  const MarketDataMessage replace{.sequence = 1U,
                                  .order_id = OrderId{11U},
                                  .quantity = Quantity{3U},
                                  .type = MarketDataMessageType::replace_order};
  EXPECT_EQ(adapter.adapt(replace).error(), MarketDataAdaptError::unsupported_message);

  const MarketDataMessage first{.sequence = 2U,
                                .order_id = OrderId{11U},
                                .price = Price{101},
                                .quantity = Quantity{4U},
                                .type = MarketDataMessageType::add_order};
  ASSERT_TRUE(adapter.adapt(first).has_value());
  const MarketDataMessage gap{.sequence = 4U,
                              .order_id = OrderId{12U},
                              .price = Price{101},
                              .quantity = Quantity{4U},
                              .type = MarketDataMessageType::add_order};
  EXPECT_EQ(adapter.adapt(gap).error(), MarketDataAdaptError::sequence_mismatch);
}

TEST(MarketDataAdapterTest, RejectsAtGatewayBeforeSequencing) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  const MarketDataMessage rejected{.sequence = 1U,
                                   .order_id = OrderId{11U},
                                   .price = Price{201},
                                   .quantity = Quantity{1U},
                                   .type = MarketDataMessageType::add_order,
                                   .side = Side::buy};
  EXPECT_EQ(adapter.adapt(rejected).error(), MarketDataAdaptError::invalid_command);
  EXPECT_EQ(adapter.last_gateway_reject_reason(), GatewayRejectReason::price_out_of_collar);

  const MarketDataMessage accepted{.sequence = 1U,
                                   .order_id = OrderId{11U},
                                   .price = Price{100},
                                   .quantity = Quantity{1U},
                                   .type = MarketDataMessageType::add_order,
                                   .side = Side::buy};
  ASSERT_TRUE(adapter.adapt(accepted).has_value());
  MarketDataMessage duplicate = accepted;
  duplicate.sequence = 2U;
  EXPECT_EQ(adapter.adapt(duplicate).error(), MarketDataAdaptError::invalid_command);
  EXPECT_EQ(adapter.last_gateway_reject_reason(), GatewayRejectReason::duplicate_order_id);
}

} // namespace matching_engine