#include "matching_engine/mbo_publisher.hpp"

#include <array>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(MboPublisherTest, EncodesCanonicalEngineEventFrame) {
  MboPublisher publisher;
  const EngineEvent event{.command_sequence = Sequence{7U},
                          .order_id = OrderId{11U},
                          .price = Price{101},
                          .quantity = Quantity{4U},
                          .handle = Handle{2U, 3U},
                          .type = EngineEventType::submit_result};
  std::array<std::byte, kEncodedEngineEventSize> output{};

  EXPECT_EQ(publisher.publish(event, output), MboPublishStatus::published);
  EXPECT_EQ(publisher.last_error(), EventCodecError::none);
  EXPECT_EQ(decode_engine_event(output), event);
}

TEST(MboPublisherTest, ReportsFrameEncodingErrors) {
  MboPublisher publisher;
  const EngineEvent event{};
  std::array<std::byte, 1U> output{};

  EXPECT_EQ(publisher.publish(event, output), MboPublishStatus::output_error);
  EXPECT_EQ(publisher.last_error(), EventCodecError::invalid_length);
}

} // namespace matching_engine