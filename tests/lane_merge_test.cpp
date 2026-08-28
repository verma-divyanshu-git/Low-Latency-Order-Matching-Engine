#include "matching_engine/lane_merge.hpp"

#include <array>

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

TEST(DeterministicLaneMergeTest, ProducesIdenticalOutputForArrivalPermutations) {
  const std::array<LaneCommand, 4U> commands{
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{1U}, Side::buy,
                                                            Quantity{1U}),
                  .logical_time = 20U,
                  .lane_id = 0U,
                  .lane_sequence = 1U},
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{2U}, Side::buy,
                                                            Quantity{1U}),
                  .logical_time = 30U,
                  .lane_id = 0U,
                  .lane_sequence = 2U},
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{3U}, Side::sell,
                                                            Quantity{1U}),
                  .logical_time = 10U,
                  .lane_id = 1U,
                  .lane_sequence = 1U},
      LaneCommand{.payload = CommandPayload::submit_market(OrderId{4U}, Side::sell,
                                                            Quantity{1U}),
                  .logical_time = 20U,
                  .lane_id = 1U,
                  .lane_sequence = 2U}};

  const auto merged_for_arrival_order = [&commands](const std::array<std::size_t, 4U>& order) {
    LaneQueue first{2U};
    LaneQueue second{2U};
    auto first_producer = first.claim_producer();
    auto second_producer = second.claim_producer();
    auto first_consumer = first.claim_consumer();
    auto second_consumer = second.claim_consumer();
    EXPECT_TRUE(first_producer && second_producer && first_consumer && second_consumer);

    for (const std::size_t index : order) {
      LaneQueue::Producer& producer = commands[index].lane_id == 0U ? *first_producer : *second_producer;
      EXPECT_TRUE(producer.try_push(commands[index]));
    }

    std::vector<LaneQueue::Consumer> consumers;
    consumers.push_back(std::move(*first_consumer));
    consumers.push_back(std::move(*second_consumer));
    DeterministicLaneMerger merger{std::move(consumers)};
    std::array<LaneCommand, 4U> merged{};
    for (LaneCommand& command : merged) {
      EXPECT_TRUE(merger.try_pop(command));
    }
    EXPECT_FALSE(merger.try_pop(merged.front()));
    return merged;
  };

  const auto baseline = merged_for_arrival_order({0U, 1U, 2U, 3U});
  EXPECT_EQ(merged_for_arrival_order({2U, 3U, 0U, 1U}), baseline);
  EXPECT_EQ(merged_for_arrival_order({0U, 2U, 1U, 3U}), baseline);
  EXPECT_EQ(merged_for_arrival_order({2U, 0U, 3U, 1U}), baseline);
  EXPECT_EQ(baseline[0].payload.order_id, 3U);
  EXPECT_EQ(baseline[1].payload.order_id, 1U);
  EXPECT_EQ(baseline[2].payload.order_id, 4U);
  EXPECT_EQ(baseline[3].payload.order_id, 2U);
}

} // namespace matching_engine