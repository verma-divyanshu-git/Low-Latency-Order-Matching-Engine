#include "matching_engine/mbp_publisher.hpp"

#include <array>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(MbpPublisherTest, PublishesAuthoritativeBestLevelForCanonicalEvent) {
  OrderBook book{PriceDomain{Price{100}, 3U}, 4U, Quantity{10U}};
  std::array<Trade, 4U> trades{};
  ASSERT_EQ(book.submit_limit(OrderId{11U}, Side::buy, Price{101}, Quantity{4U}, trades).reject_reason,
            RejectReason::none);
  MbpPublisher publisher;
  std::array<std::byte, kEncodedMarketDataFrameSize> output{};
  const EngineEvent event{.command_sequence = Sequence{7U}, .type = EngineEventType::submit_result};

  EXPECT_EQ(publisher.publish_best(event, book, Side::buy, output), MbpPublishStatus::published);
  EXPECT_EQ(decode_market_data_frame(output),
            (MarketDataMessage{.sequence = 7U,
                               .price = Price{101},
                               .quantity = Quantity{4U},
                               .order_count = 1U,
                               .type = MarketDataMessageType::level_update,
                               .side = Side::buy}));
}

TEST(MbpPublisherTest, PublishesEmptyBestLevelAfterCanonicalEvent) {
  OrderBook book{PriceDomain{Price{100}, 3U}, 4U, Quantity{10U}};
  MbpPublisher publisher;
  std::array<std::byte, kEncodedMarketDataFrameSize> output{};
  const EngineEvent event{.command_sequence = Sequence{8U}, .type = EngineEventType::cancel_result};

  EXPECT_EQ(publisher.publish_best(event, book, Side::sell, output), MbpPublishStatus::published);
  EXPECT_EQ(decode_market_data_frame(output),
            (MarketDataMessage{.sequence = 8U,
                               .type = MarketDataMessageType::level_update,
                               .side = Side::sell}));
}

TEST(MbpPublisherTest, ReportsFrameEncodingErrors) {
  OrderBook book{PriceDomain{Price{100}, 1U}, 0U, Quantity{1U}};
  MbpPublisher publisher;
  std::array<std::byte, 1U> output{};

  EXPECT_EQ(publisher.publish_best(EngineEvent{}, book, Side::buy, output),
            MbpPublishStatus::output_error);
  EXPECT_EQ(publisher.last_error(), MarketDataFrameError::invalid_length);
}

} // namespace matching_engine