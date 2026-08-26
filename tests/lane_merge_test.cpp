#include "matching_engine/lane_merge.hpp"

#include <gtest/gtest.h>

namespace matching_engine {

TEST(DeterministicLaneMergeTest, OrdersByLogicalTimeThenLaneAndSequence) {
  LaneQueue first{4U};
  LaneQueue second{4U};
  auto first_producer = first.claim_producer();
  auto second_producer = second.claim_producer();
  auto first_consumer = first.claim_consumer();
  auto second_consumer = second.claim_consumer();
  ASSERT_TRUE(first_producer.has_value());
  ASSERT_TRUE(second_producer.has_value());
  ASSERT_TRUE(first_consumer.has_value());
  ASSERT_TRUE(second_consumer.has_value());

  ASSERT_TRUE(first_producer->try_push(
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{1U}, Side::buy,
                                                            Quantity{1U}),
                   .logical_time = 20U,
                   .lane_id = 1U,
                   .lane_sequence = 1U}));
  ASSERT_TRUE(first_producer->try_push(
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{2U}, Side::buy,
                                                            Quantity{1U}),
                   .logical_time = 30U,
                   .lane_id = 1U,
                   .lane_sequence = 2U}));
  ASSERT_TRUE(second_producer->try_push(
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{3U}, Side::buy,
                                                            Quantity{1U}),
                   .logical_time = 20U,
                   .lane_id = 0U,
                   .lane_sequence = 1U}));

  std::vector<LaneQueue::Consumer> consumers;
  consumers.push_back(std::move(*first_consumer));
  consumers.push_back(std::move(*second_consumer));
  DeterministicLaneMerger merger{std::move(consumers)};

  LaneCommand command{};
  ASSERT_TRUE(merger.try_pop(command));
  EXPECT_EQ(command.payload.order_id, 3U);
  ASSERT_TRUE(merger.try_pop(command));
  EXPECT_EQ(command.payload.order_id, 1U);
  ASSERT_TRUE(merger.try_pop(command));
  EXPECT_EQ(command.payload.order_id, 2U);
  EXPECT_FALSE(merger.try_pop(command));
}

TEST(DeterministicLaneMergeTest, PreservesLaneFifoAndRejectsOversizedQueue) {
  EXPECT_THROW(LaneQueue{0U}, std::invalid_argument);

  LaneQueue lane{1U};
  auto producer = lane.claim_producer();
  ASSERT_TRUE(producer.has_value());
  const LaneCommand command{
      .payload = CommandPayload::submit_market(OrderId{10U}, Side::sell, Quantity{1U}),
      .logical_time = 1U,
      .lane_id = 0U,
      .lane_sequence = 1U};
  EXPECT_TRUE(producer->try_push(command));
  EXPECT_FALSE(producer->try_push(command));
}

} // namespace matching_engine