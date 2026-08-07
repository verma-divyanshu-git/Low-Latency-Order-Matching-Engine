#include "matching_engine/order_arena.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <type_traits>

namespace matching_engine {
namespace {

static_assert(!std::is_convertible_v<std::int64_t, Price>);
static_assert(!std::is_convertible_v<std::uint64_t, Quantity>);
static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
static_assert(!std::is_convertible_v<std::uint64_t, Sequence>);
static_assert(!std::is_convertible_v<Price, std::int64_t>);
static_assert(!std::is_convertible_v<Quantity, std::uint64_t>);
static_assert(!std::is_convertible_v<OrderId, std::uint64_t>);
static_assert(!std::is_convertible_v<Sequence, std::uint64_t>);
static_assert(!std::is_copy_constructible_v<OrderArena>);
static_assert(!std::is_copy_assignable_v<OrderArena>);
static_assert(!std::is_move_constructible_v<OrderArena>);
static_assert(!std::is_move_assignable_v<OrderArena>);

constexpr Order make_order(std::uint64_t id = 7, std::uint64_t remaining = 11) {
  return Order{
      .id = OrderId{id},
      .remaining = Quantity{remaining},
      .prev_index = kInvalidIndex,
      .next_index = kInvalidIndex,
      .encoded_level_side = 23,
      .reserved_flags = 5,
  };
}

TEST(OrderArenaTest, CapacityZeroIsSafelyExhausted) {
  OrderArena arena{0};

  EXPECT_EQ(arena.capacity(), 0U);
  EXPECT_EQ(arena.size(), 0U);
  EXPECT_EQ(arena.acquire(make_order()).error, ArenaError::full);
}

TEST(OrderArenaTest, ReportsExhaustionWithoutGrowing) {
  OrderArena arena{1};

  const AcquireResult first = arena.acquire(make_order());
  const AcquireResult second = arena.acquire(make_order(8));

  ASSERT_EQ(first.error, ArenaError::none);
  EXPECT_EQ(second.error, ArenaError::full);
  EXPECT_EQ(arena.capacity(), 1U);
  EXPECT_EQ(arena.size(), 1U);
}

TEST(OrderArenaTest, ReusesReleasedSlotWithNewGeneration) {
  OrderArena arena{1};
  const AcquireResult first = arena.acquire(make_order());
  ASSERT_EQ(first.error, ArenaError::none);
  ASSERT_EQ(arena.release(first.handle), ArenaError::none);

  const AcquireResult reused = arena.acquire(make_order(8));

  ASSERT_EQ(reused.error, ArenaError::none);
  EXPECT_EQ(reused.handle.index, first.handle.index);
  EXPECT_NE(reused.handle.generation, first.handle.generation);
  EXPECT_NE(reused.handle.generation, 0U);
}

TEST(OrderArenaTest, RejectsStaleHandleAfterReuse) {
  OrderArena arena{1};
  const AcquireResult first = arena.acquire(make_order());
  ASSERT_EQ(first.error, ArenaError::none);
  ASSERT_EQ(arena.release(first.handle), ArenaError::none);
  ASSERT_EQ(arena.acquire(make_order(8)).error, ArenaError::none);

  EXPECT_EQ(arena.resolve(first.handle), nullptr);
  EXPECT_EQ(arena.release(first.handle), ArenaError::invalid_handle);
}

TEST(OrderArenaTest, RejectsDoubleRelease) {
  OrderArena arena{1};
  const AcquireResult acquired = arena.acquire(make_order());
  ASSERT_EQ(acquired.error, ArenaError::none);

  EXPECT_EQ(arena.release(acquired.handle), ArenaError::none);
  EXPECT_EQ(arena.release(acquired.handle), ArenaError::invalid_handle);
}

TEST(OrderArenaTest, RejectsOutOfRangeHandle) {
  OrderArena arena{1};
  const Handle out_of_range{.index = 1, .generation = 1};

  EXPECT_EQ(arena.resolve(out_of_range), nullptr);
  EXPECT_EQ(arena.release(out_of_range), ArenaError::invalid_handle);
}

TEST(OrderArenaTest, PreservesEveryStoredOrderField) {
  OrderArena arena{1};
  const Order expected = make_order();
  const AcquireResult acquired = arena.acquire(expected);
  ASSERT_EQ(acquired.error, ArenaError::none);

  const Order* const stored = arena.resolve(acquired.handle);

  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->id, expected.id);
  EXPECT_EQ(stored->remaining, expected.remaining);
  EXPECT_EQ(stored->prev_index, expected.prev_index);
  EXPECT_EQ(stored->next_index, expected.next_index);
  EXPECT_EQ(stored->encoded_level_side, expected.encoded_level_side);
  EXPECT_EQ(stored->reserved_flags, expected.reserved_flags);
}

TEST(OrderArenaTest, ConstructionFixesCapacityAcrossReuse) {
  OrderArena arena{2};
  const std::uint32_t initial_capacity = arena.capacity();
  const AcquireResult acquired = arena.acquire(make_order());
  ASSERT_EQ(acquired.error, ArenaError::none);
  ASSERT_EQ(arena.release(acquired.handle), ArenaError::none);
  ASSERT_EQ(arena.acquire(make_order(8)).error, ArenaError::none);

  EXPECT_EQ(arena.capacity(), initial_capacity);
}

} // namespace
} // namespace matching_engine
