#include "matching_engine/runtime_operations.hpp"

#include <gtest/gtest.h>

namespace matching_engine {

TEST(RuntimeOperationsTest, StartsOnceAndStopsOnlyAfterBothQueuesDrain) {
  RuntimeOperations operations;
  EXPECT_FALSE(operations.accepts_ingress());
  EXPECT_TRUE(operations.start());
  EXPECT_FALSE(operations.start());
  EXPECT_TRUE(operations.accepts_ingress());
  EXPECT_TRUE(operations.begin_shutdown());
  EXPECT_FALSE(operations.accepts_ingress());
  EXPECT_FALSE(operations.complete_shutdown(false, true));
  EXPECT_FALSE(operations.complete_shutdown(true, false));
  EXPECT_TRUE(operations.complete_shutdown(true, true));
  EXPECT_EQ(operations.snapshot().lifecycle, RuntimeLifecycle::stopped);
}

TEST(RuntimeOperationsTest, ReportsBackpressureCapacityAndMonotonicCounters) {
  RuntimeOperations operations;
  ASSERT_TRUE(operations.start());
  operations.observe(IngressStatus::progress);
  operations.observe(MatchingStatus::progress);
  operations.observe(PublicationStatus::progress);
  operations.observe(IngressStatus::queue_backpressure);
  operations.observe(MatchingStatus::output_backpressure);
  operations.observe(IngressStatus::journal_full);

  const RuntimeHealthSnapshot snapshot = operations.snapshot();
  EXPECT_EQ(snapshot.health, RuntimeHealth::capacity_exhausted);
  EXPECT_EQ(snapshot.accepted_commands, 1U);
  EXPECT_EQ(snapshot.matched_commands, 1U);
  EXPECT_EQ(snapshot.published_events, 1U);
  EXPECT_EQ(snapshot.backpressure_events, 2U);
  EXPECT_EQ(snapshot.capacity_events, 1U);
}

TEST(RuntimeOperationsTest, RecoveryAndCorruptionFailuresStopIngress) {
  RuntimeOperations recovery;
  ASSERT_TRUE(recovery.start());
  recovery.observe(IngressStatus::commit_indeterminate);
  EXPECT_EQ(recovery.snapshot().lifecycle, RuntimeLifecycle::failed);
  EXPECT_EQ(recovery.snapshot().health, RuntimeHealth::recovery_required);
  EXPECT_FALSE(recovery.accepts_ingress());

  RuntimeOperations corruption;
  ASSERT_TRUE(corruption.start());
  corruption.observe(MatchingStatus::invalid_sequence);
  EXPECT_EQ(corruption.snapshot().lifecycle, RuntimeLifecycle::failed);
  EXPECT_EQ(corruption.snapshot().health, RuntimeHealth::corruption);
  EXPECT_FALSE(corruption.accepts_ingress());
}

TEST(RuntimeOperationsTest, StructuredReportHasFixedSchemaAndNoExternalText) {
  RuntimeOperations operations;
  ASSERT_TRUE(operations.start());
  operations.observe(IngressStatus::progress);
  const auto json = runtime_health_json(operations.snapshot());
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(*json,
            "{\"schema_version\":1,\"lifecycle\":\"running\",\"health\":\"healthy\","
            "\"accepted_commands\":1,\"matched_commands\":0,\"published_events\":0,"
            "\"backpressure_events\":0,\"capacity_events\":0,\"failure_events\":0}");
}

} // namespace matching_engine
