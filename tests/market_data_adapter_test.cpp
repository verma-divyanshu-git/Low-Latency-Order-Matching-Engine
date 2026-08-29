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
  EXPECT_EQ(adapter.adapt(replace).error(), MarketDataAdaptError::unknown_order);

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

TEST(MarketDataAdapterTest, MapsDeletesToRecordedGenerationHandle) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  const MarketDataMessage add{.sequence = 1U,
                              .order_id = OrderId{11U},
                              .price = Price{101},
                              .quantity = Quantity{1U},
                              .type = MarketDataMessageType::add_order,
                              .side = Side::buy};
  ASSERT_TRUE(adapter.adapt(add).has_value());
  adapter.record_applied_event(
      {.order_id = OrderId{11U}, .handle = Handle{3U, 7U}, .type = EngineEventType::submit_result});

  const MarketDataMessage remove{.sequence = 2U,
                                 .order_id = OrderId{11U},
                                 .type = MarketDataMessageType::delete_order};
  const auto command = adapter.adapt(remove);
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->payload, CommandPayload::cancel(Handle{3U, 7U}));

  adapter.record_applied_event(
      {.order_id = OrderId{11U}, .handle = Handle{3U, 7U}, .type = EngineEventType::cancel_result});
  const MarketDataMessage reused{.sequence = 3U,
                                  .order_id = OrderId{11U},
                                  .price = Price{101},
                                  .quantity = Quantity{1U},
                                  .type = MarketDataMessageType::add_order,
                                  .side = Side::buy};
  EXPECT_TRUE(adapter.adapt(reused).has_value());
}

TEST(MarketDataAdapterTest, MapsReplacesToRecordedGenerationHandle) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  adapter.record_applied_event(
      {.order_id = OrderId{11U}, .handle = Handle{3U, 7U}, .type = EngineEventType::submit_result});
  const MarketDataMessage replace{.sequence = 1U,
                                  .order_id = OrderId{11U},
                                  .quantity = Quantity{2U},
                                  .type = MarketDataMessageType::replace_order};
  const auto command = adapter.adapt(replace);
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->payload, CommandPayload::amend_quantity(Handle{3U, 7U}, Quantity{2U}));

  const MarketDataMessage unknown{.sequence = 2U,
                                  .order_id = OrderId{12U},
                                  .quantity = Quantity{2U},
                                  .type = MarketDataMessageType::replace_order};
  EXPECT_EQ(adapter.adapt(unknown).error(), MarketDataAdaptError::unknown_order);
}

TEST(MarketDataAdapterTest, StopTriggerReleasesOrUpdatesRecordedHandle) {
  MarketDataAdapter adapter{GatewayValidator{gateway_config()}};
  const MarketDataMessage first{.sequence = 1U,
                                .order_id = OrderId{11U},
                                .price = Price{101},
                                .quantity = Quantity{1U},
                                .type = MarketDataMessageType::add_order,
                                .side = Side::buy};
  ASSERT_TRUE(adapter.adapt(first).has_value());
  adapter.record_applied_event(
      {.order_id = OrderId{11U}, .handle = Handle{3U, 7U}, .type = EngineEventType::submit_result});
  adapter.record_applied_event({.order_id = OrderId{11U},
                                .handle = Handle{4U, 8U},
                                .type = EngineEventType::stop_triggered});

  const MarketDataMessage remove{.sequence = 2U,
                                 .order_id = OrderId{11U},
                                 .type = MarketDataMessageType::delete_order};
  const auto remove_command = adapter.adapt(remove);
  ASSERT_TRUE(remove_command.has_value());
  EXPECT_EQ(remove_command->payload, CommandPayload::cancel(Handle{4U, 8U}));

  adapter.record_applied_event({.order_id = OrderId{11U},
                                .handle = Handle{kInvalidIndex, 0U},
                                .type = EngineEventType::stop_triggered});
  MarketDataMessage reused = first;
  reused.sequence = 3U;
  EXPECT_TRUE(adapter.adapt(reused).has_value());
}

} // namespace matching_engine