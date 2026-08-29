#include "matching_engine/runtime_operations.hpp"

#include <gtest/gtest.h>

namespace matching_engine {
namespace {

RuntimeHealthSnapshot run_recovery_schedule(std::uint64_t failure_step) {
  RuntimeOperations operations;
  if (!operations.start()) {
    return operations.snapshot();
  }
  for (std::uint64_t step = 0U; step < 1'000U; ++step) {
    if (step == failure_step) {
      operations.observe(IngressStatus::commit_indeterminate);
      break;
    }
    operations.observe(IngressStatus::progress);
    operations.observe(MatchingStatus::progress);
    operations.observe(PublicationStatus::progress);
  }
  return operations.snapshot();
}

TEST(RuntimeReliabilityTest, HundredThousandCycleSoakHasExactFaultAccounting) {
  RuntimeOperations operations;
  ASSERT_TRUE(operations.start());
  constexpr std::uint64_t cycles = 100'000U;
  for (std::uint64_t step = 1U; step <= cycles; ++step) {
    operations.observe(IngressStatus::progress);
    operations.observe(MatchingStatus::progress);
    operations.observe(PublicationStatus::progress);
    if (step % 1'000U == 0U) {
      operations.observe(IngressStatus::queue_backpressure);
    }
    if (step % 2'500U == 0U) {
      operations.observe(MatchingStatus::output_backpressure);
    }
    if (step % 10'000U == 0U) {
      operations.observe(IngressStatus::journal_full);
    }
  }

  ASSERT_TRUE(operations.begin_shutdown());
  ASSERT_TRUE(operations.complete_shutdown(true, true));
  const RuntimeHealthSnapshot snapshot = operations.snapshot();
  EXPECT_EQ(snapshot.lifecycle, RuntimeLifecycle::stopped);
  EXPECT_EQ(snapshot.health, RuntimeHealth::capacity_exhausted);
  EXPECT_EQ(snapshot.accepted_commands, cycles);
  EXPECT_EQ(snapshot.matched_commands, cycles);
  EXPECT_EQ(snapshot.published_events, cycles);
  EXPECT_EQ(snapshot.backpressure_events, 140U);
  EXPECT_EQ(snapshot.capacity_events, 10U);
  EXPECT_EQ(snapshot.failure_events, 0U);
}

TEST(RuntimeReliabilityTest, RecoveryFaultScheduleIsExactlyReproducible) {
  const RuntimeHealthSnapshot first = run_recovery_schedule(777U);
  const RuntimeHealthSnapshot second = run_recovery_schedule(777U);

  EXPECT_EQ(first, second);
  EXPECT_EQ(first.lifecycle, RuntimeLifecycle::failed);
  EXPECT_EQ(first.health, RuntimeHealth::recovery_required);
  EXPECT_EQ(first.accepted_commands, 777U);
  EXPECT_EQ(first.matched_commands, 777U);
  EXPECT_EQ(first.published_events, 777U);
  EXPECT_EQ(first.failure_events, 1U);
}

TEST(RuntimeReliabilityTest, CorruptionFaultDominatesEarlierTransientHealth) {
  RuntimeOperations operations;
  ASSERT_TRUE(operations.start());
  operations.observe(IngressStatus::queue_backpressure);
  operations.observe(IngressStatus::journal_full);
  operations.observe(MatchingStatus::internal_invariant_failure);

  const RuntimeHealthSnapshot snapshot = operations.snapshot();
  EXPECT_EQ(snapshot.lifecycle, RuntimeLifecycle::failed);
  EXPECT_EQ(snapshot.health, RuntimeHealth::corruption);
  EXPECT_EQ(snapshot.backpressure_events, 1U);
  EXPECT_EQ(snapshot.capacity_events, 1U);
  EXPECT_EQ(snapshot.failure_events, 1U);
  EXPECT_FALSE(operations.accepts_ingress());
}

} // namespace
} // namespace matching_engine
