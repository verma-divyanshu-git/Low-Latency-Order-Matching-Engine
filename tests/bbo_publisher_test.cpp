#include "matching_engine/bbo_publisher.hpp"

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(BboPublisherTest, PublishesChangesAndSuppressesUnchangedSnapshots) {
  BboSnapshot snapshot;
  BboPublisher publisher;
  std::array<std::byte, kEncodedBboFrameSize> encoded{};
  snapshot.publish(BboState{.bid_price = Price{100},
                            .bid_quantity = Quantity{4U},
                            .ask_price = Price{101},
                            .ask_quantity = Quantity{6U}});

  EXPECT_EQ(publisher.try_publish(snapshot, encoded), BboPublishStatus::published);
  const BboState expected{.bid_price = Price{100},
                          .bid_quantity = Quantity{4U},
                          .ask_price = Price{101},
                          .ask_quantity = Quantity{6U}};
  EXPECT_EQ(decode_bbo_frame(encoded), expected);
  EXPECT_EQ(publisher.try_publish(snapshot, encoded), BboPublishStatus::unchanged);

  snapshot.publish(BboState{.bid_price = Price{100},
                            .bid_quantity = Quantity{3U},
                            .ask_price = std::nullopt,
                            .ask_quantity = Quantity{0U}});
  EXPECT_EQ(publisher.try_publish(snapshot, encoded), BboPublishStatus::published);
}

TEST(BboPublisherTest, ReportsOutputFailuresWithoutAdvancingPublishedState) {
  BboSnapshot snapshot;
  BboPublisher publisher;
  std::array<std::byte, kEncodedBboFrameSize> encoded{};
  snapshot.publish(BboState{.bid_price = Price{100}, .bid_quantity = Quantity{4U}});

  EXPECT_EQ(publisher.try_publish(snapshot, std::span<std::byte>{encoded.data(), 1U}),
            BboPublishStatus::output_error);
  EXPECT_EQ(publisher.last_frame_error(), BboFrameError::invalid_length);
  EXPECT_EQ(publisher.try_publish(snapshot, encoded), BboPublishStatus::published);
}

} // namespace matching_engine