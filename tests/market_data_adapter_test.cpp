#include "matching_engine/market_data_adapter.hpp"

#include <gtest/gtest.h>

namespace matching_engine {

TEST(MarketDataAdapterTest, AdaptsContiguousAddOrdersToEngineCommands) {
  MarketDataAdapter adapter;
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
  MarketDataAdapter adapter;
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

} // namespace matching_engine