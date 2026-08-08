#include "matching_engine/spsc_queue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <stop_token>
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

TEST(SpscQueueBatchTest, EmptyBatchSucceedsWithoutPublishing) {
  SpscQueue<std::uint64_t> queue{2U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer && consumer);

  EXPECT_TRUE(producer->try_push_batch(std::span<const std::uint64_t>{}));
  EXPECT_TRUE(queue.empty());
  std::uint64_t value{};
  EXPECT_FALSE(consumer->try_pop(value));
}

TEST(SpscQueueBatchTest, RequiresCompleteCapacityAndPublishesExactFullBatch) {
  SpscQueue<std::uint64_t> queue{4U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer && consumer);
  const std::array<std::uint64_t, 4U> full{1U, 2U, 3U, 4U};

  ASSERT_TRUE(producer->try_push(99U));
  EXPECT_FALSE(producer->try_push_batch(full));
  std::uint64_t value{};
  ASSERT_TRUE(consumer->try_pop(value));
  EXPECT_EQ(value, 99U);
  ASSERT_TRUE(producer->try_push_batch(full));
  EXPECT_EQ(producer->available(), 0U);
  for (const std::uint64_t expected : full) {
    ASSERT_TRUE(consumer->try_pop(value));
    EXPECT_EQ(value, expected);
  }
}

TEST(SpscQueueBatchTest, InitializesWrappedSlotsBeforeSinglePublication) {
  SpscQueue<std::uint64_t> queue{5U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer && consumer);
  const std::array<std::uint64_t, 4U> prefix{1U, 2U, 3U, 4U};
  ASSERT_TRUE(producer->try_push_batch(prefix));
  std::uint64_t value{};
  for (std::size_t index = 0U; index < 3U; ++index) {
    ASSERT_TRUE(consumer->try_pop(value));
  }
  const std::array<std::uint64_t, 4U> wrapped{5U, 6U, 7U, 8U};

  ASSERT_TRUE(producer->try_push_batch(wrapped));

  const std::array<std::uint64_t, 5U> expected{4U, 5U, 6U, 7U, 8U};
  for (const std::uint64_t item : expected) {
    ASSERT_TRUE(consumer->try_pop(value));
    EXPECT_EQ(value, item);
  }
}

TEST(SpscQueueBatchTest, ConsumerNeverObservesPartialPublishedBatch) {
  struct Item {
    std::uint64_t batch{};
    std::uint64_t offset{};
    constexpr bool operator==(const Item&) const noexcept = default;
  };
  constexpr std::size_t batch_size = 8U;
  constexpr std::uint64_t batch_count = 100'000U;
  SpscQueue<Item> queue{31U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  ASSERT_TRUE(producer && consumer);
  std::stop_source stop;
  std::atomic<bool> failed{};
  std::atomic<bool> done{};
  std::jthread watchdog{[&](const std::stop_token& token) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!token.stop_requested() && !done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!done.load(std::memory_order_acquire) && !token.stop_requested()) {
      failed.store(true, std::memory_order_release);
      stop.request_stop();
    }
  }};

  std::jthread producer_thread{[endpoint = std::move(*producer), &stop]() mutable {
    std::array<Item, batch_size> batch{};
    for (std::uint64_t batch_id = 0U; batch_id < batch_count && !stop.stop_requested();
         ++batch_id) {
      for (std::size_t offset = 0U; offset < batch.size(); ++offset) {
        batch[offset] = {batch_id, offset};
      }
      while (!endpoint.try_push_batch(batch) && !stop.stop_requested()) {
        std::this_thread::yield();
      }
    }
  }};
  std::jthread consumer_thread{[endpoint = std::move(*consumer), &stop, &failed, &done]() mutable {
    for (std::uint64_t batch_id = 0U; batch_id < batch_count && !stop.stop_requested();
         ++batch_id) {
      Item item{};
      while (!endpoint.try_pop(item) && !stop.stop_requested()) {
        std::this_thread::yield();
      }
      if (stop.stop_requested()) {
        break;
      }
      if (item != Item{batch_id, 0U}) {
        failed.store(true, std::memory_order_release);
        stop.request_stop();
        break;
      }
      for (std::size_t offset = 1U; offset < batch_size; ++offset) {
        if (!endpoint.try_pop(item) || item != Item{batch_id, offset}) {
          failed.store(true, std::memory_order_release);
          stop.request_stop();
          break;
        }
      }
    }
    done.store(true, std::memory_order_release);
  }};
  producer_thread.join();
  consumer_thread.join();
  done.store(true, std::memory_order_release);
  watchdog.request_stop();
  watchdog.join();
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
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
  std::atomic<bool> failed{};
  std::atomic<bool> done{};
  std::stop_source stop;
  constexpr std::uint64_t count = 1'000'000U;
  std::jthread watchdog{[&](const std::stop_token& token) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!token.stop_requested() && !done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!done.load(std::memory_order_acquire) && !token.stop_requested()) {
      failed.store(true, std::memory_order_release);
      stop.request_stop();
    }
  }};

  std::jthread producer_thread{[endpoint = std::move(*producer), &start, &stop]() mutable {
    while (!start.load(std::memory_order_acquire) && !stop.stop_requested()) {
      std::this_thread::yield();
    }
    for (std::uint64_t value = 0U; value < count && !stop.stop_requested(); ++value) {
      const Payload payload{value, ~value};
      while (!endpoint.try_push(payload) && !stop.stop_requested()) {
        std::this_thread::yield();
      }
    }
  }};
  std::jthread consumer_thread{
      [endpoint = std::move(*consumer), &start, &stop, &failed, &done]() mutable {
        start.store(true, std::memory_order_release);
        for (std::uint64_t expected = 0U; expected < count && !stop.stop_requested(); ++expected) {
          Payload payload{};
          while (!endpoint.try_pop(payload) && !stop.stop_requested()) {
            std::this_thread::yield();
          }
          if (stop.stop_requested()) {
            break;
          }
          if (payload.sequence != expected || payload.complement != ~expected) {
            failed.store(true, std::memory_order_release);
            stop.request_stop();
            break;
          }
        }
        done.store(true, std::memory_order_release);
      }};
  producer_thread.join();
  consumer_thread.join();
  done.store(true, std::memory_order_release);
  watchdog.request_stop();
  watchdog.join();
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
}

TEST(SpscQueueTest, RejectsInvalidCapacity) {
  EXPECT_THROW((SpscQueue<std::uint64_t>{0U}), std::invalid_argument);
  EXPECT_THROW((SpscQueue<std::uint64_t>{kMaximumSpscCapacity + 1U}), std::invalid_argument);
}

} // namespace
} // namespace matching_engine
