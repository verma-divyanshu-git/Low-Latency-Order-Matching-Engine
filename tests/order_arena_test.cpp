#include "matching_engine/order_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
static_assert(noexcept(std::declval<OrderArena&>().acquire(std::declval<const Order&>())));
static_assert(noexcept(std::declval<OrderArena&>().resolve(std::declval<Handle>())));
static_assert(noexcept(std::declval<OrderArena&>().release(std::declval<Handle>())));
static_assert(noexcept(std::declval<const OrderArena&>().capacity()));
static_assert(noexcept(std::declval<const OrderArena&>().size()));
static_assert(detail::next_generation(std::numeric_limits<std::uint32_t>::max()) == 1U);
static_assert(detail::next_generation(1U) == 2U);

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

TEST(OrderArenaTest, RejectsCapacityBeforeNarrowing) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
    constexpr std::size_t too_large =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;

    EXPECT_THROW((OrderArena{too_large}), std::length_error);
  }
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

TEST(OrderArenaTest, RejectsGenerationZeroHandle) {
  OrderArena arena{1};
  const AcquireResult acquired = arena.acquire(make_order());
  ASSERT_EQ(acquired.error, ArenaError::none);
  const Handle generation_zero{.index = acquired.handle.index, .generation = 0U};

  EXPECT_EQ(arena.resolve(generation_zero), nullptr);
  EXPECT_EQ(arena.release(generation_zero), ArenaError::invalid_handle);
  EXPECT_NE(arena.resolve(acquired.handle), nullptr);
}

TEST(OrderArenaTest, PreservesFreeListAcrossNonLifoReuse) {
  OrderArena arena{3};
  const AcquireResult first = arena.acquire(make_order(1));
  const AcquireResult second = arena.acquire(make_order(2));
  const AcquireResult third = arena.acquire(make_order(3));
  ASSERT_EQ(first.error, ArenaError::none);
  ASSERT_EQ(second.error, ArenaError::none);
  ASSERT_EQ(third.error, ArenaError::none);

  ASSERT_EQ(arena.release(first.handle), ArenaError::none);
  ASSERT_EQ(arena.release(third.handle), ArenaError::none);
  const AcquireResult reused_third = arena.acquire(make_order(4));
  ASSERT_EQ(arena.release(second.handle), ArenaError::none);
  const AcquireResult reused_second = arena.acquire(make_order(5));
  const AcquireResult reused_first = arena.acquire(make_order(6));

  ASSERT_EQ(reused_third.error, ArenaError::none);
  ASSERT_EQ(reused_second.error, ArenaError::none);
  ASSERT_EQ(reused_first.error, ArenaError::none);
  EXPECT_EQ(reused_third.handle.index, third.handle.index);
  EXPECT_EQ(reused_second.handle.index, second.handle.index);
  EXPECT_EQ(reused_first.handle.index, first.handle.index);
  EXPECT_EQ(arena.size(), 3U);
  EXPECT_EQ(arena.acquire(make_order(7)).error, ArenaError::full);
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

TEST(OrderArenaTest, KeepsResolvedAddressStableWhileHandleIsLive) {
  OrderArena arena{2};
  const AcquireResult retained = arena.acquire(make_order(1));
  ASSERT_EQ(retained.error, ArenaError::none);
  Order* const initial_address = arena.resolve(retained.handle);
  const AcquireResult recycled = arena.acquire(make_order(2));
  ASSERT_EQ(recycled.error, ArenaError::none);
  ASSERT_EQ(arena.release(recycled.handle), ArenaError::none);
  ASSERT_EQ(arena.acquire(make_order(3)).error, ArenaError::none);

  EXPECT_EQ(arena.resolve(retained.handle), initial_address);
}

} // namespace
} // namespace matching_engine
