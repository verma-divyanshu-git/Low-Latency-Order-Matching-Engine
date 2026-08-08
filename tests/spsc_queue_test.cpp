#include "matching_engine/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <type_traits>

namespace matching_engine {
namespace {

static_assert(!std::is_copy_constructible_v<SpscQueue<std::uint64_t>::Producer>);
static_assert(!std::is_copy_constructible_v<SpscQueue<std::uint64_t>::Consumer>);
static_assert(std::is_move_constructible_v<SpscQueue<std::uint64_t>::Producer>);
static_assert(std::is_move_constructible_v<SpscQueue<std::uint64_t>::Consumer>);

TEST(SpscQueueTest, CapacityOneHasExactFullEmptyBoundaries) {
  SpscQueue<std::uint64_t> queue{1U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer.has_value());
  ASSERT_TRUE(consumer.has_value());
  EXPECT_FALSE(queue.claim_producer().has_value());
  EXPECT_FALSE(queue.claim_consumer().has_value());
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(producer->available(), 1U);
  EXPECT_TRUE(producer->try_push(7U));
  EXPECT_EQ(producer->available(), 0U);
  EXPECT_FALSE(producer->try_push(8U));
  EXPECT_FALSE(queue.empty());

  std::uint64_t value{};
  EXPECT_TRUE(consumer->try_peek(value));
  EXPECT_EQ(value, 7U);
  EXPECT_TRUE(consumer->try_pop(value));
  EXPECT_EQ(value, 7U);
  EXPECT_FALSE(consumer->try_pop(value));
  EXPECT_TRUE(queue.empty());
}

TEST(SpscQueueTest, PreservesFifoAcrossManyWraps) {
  SpscQueue<std::uint64_t> queue{3U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer.has_value());
  ASSERT_TRUE(consumer.has_value());

  for (std::uint64_t base = 0U; base < 30'000U; base += 3U) {
    EXPECT_TRUE(producer->try_push(base));
    EXPECT_TRUE(producer->try_push(base + 1U));
    EXPECT_TRUE(producer->try_push(base + 2U));
    EXPECT_FALSE(producer->try_push(base + 3U));
    for (std::uint64_t offset = 0U; offset < 3U; ++offset) {
      std::uint64_t value{};
      ASSERT_TRUE(consumer->try_pop(value));
      EXPECT_EQ(value, base + offset);
    }
  }
}

TEST(SpscQueueTest, UnsignedIndicesRemainCorrectAcrossUint64Wrap) {
  SpscQueue<std::uint64_t> queue{4U};
  queue.set_empty_index_for_testing(std::numeric_limits<std::uint64_t>::max() - 1U);
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer.has_value());
  ASSERT_TRUE(consumer.has_value());

  for (std::uint64_t expected = 1U; expected <= 8U; ++expected) {
    ASSERT_TRUE(producer->try_push(expected));
    std::uint64_t actual{};
    ASSERT_TRUE(consumer->try_pop(actual));
    EXPECT_EQ(actual, expected);
  }
}

TEST(SpscQueueTest, TransfersOneMillionVisiblePayloads) {
  struct Payload {
    std::uint64_t sequence{};
    std::uint64_t complement{};
  };
  static_assert(std::is_trivially_copyable_v<Payload>);

  SpscQueue<Payload> queue{1024U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer.has_value());
  ASSERT_TRUE(consumer.has_value());
  std::atomic<bool> start{};
  constexpr std::uint64_t count = 1'000'000U;

  std::jthread producer_thread{[endpoint = std::move(*producer), &start]() mutable {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::uint64_t value = 0U; value < count; ++value) {
      const Payload payload{value, ~value};
      while (!endpoint.try_push(payload)) {
        std::this_thread::yield();
      }
    }
  }};
  std::jthread consumer_thread{[endpoint = std::move(*consumer), &start]() mutable {
    start.store(true, std::memory_order_release);
    for (std::uint64_t expected = 0U; expected < count; ++expected) {
      Payload payload{};
      while (!endpoint.try_pop(payload)) {
        std::this_thread::yield();
      }
      ASSERT_EQ(payload.sequence, expected);
      ASSERT_EQ(payload.complement, ~expected);
    }
  }};
}

TEST(SpscQueueTest, RejectsInvalidCapacity) {
  EXPECT_THROW((SpscQueue<std::uint64_t>{0U}), std::invalid_argument);
  EXPECT_THROW((SpscQueue<std::uint64_t>{kMaximumSpscCapacity + 1U}), std::invalid_argument);
}

} // namespace
} // namespace matching_engine
