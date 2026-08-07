#include "matching_engine/hierarchical_bitmap.hpp"
#include "matching_engine/price_domain.hpp"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace matching_engine {
namespace {

static_assert(noexcept(std::declval<const PriceDomain&>().contains(Price{0})));
static_assert(noexcept(std::declval<const PriceDomain&>().index_of(Price{0})));
static_assert(noexcept(std::declval<const PriceDomain&>().price_at(0U)));
static_assert(noexcept(std::declval<HierarchicalBitmap&>().set(0U)));
static_assert(noexcept(std::declval<HierarchicalBitmap&>().clear(0U)));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().test(0U)));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().first_set()));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().last_set()));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().next_set(0U)));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().previous_set(0U)));
static_assert(noexcept(std::declval<const HierarchicalBitmap&>().hierarchy_consistent()));

TEST(PriceDomainTest, RejectsEmptyAndOverflowingDomains) {
  EXPECT_THROW((PriceDomain{Price{0}, 0U}), std::invalid_argument);
  EXPECT_THROW((PriceDomain{Price{std::numeric_limits<std::int64_t>::max() - 1}, 3U}),
               std::overflow_error);
}

TEST(PriceDomainTest, MapsInclusiveBoundariesWithoutFloatingPoint) {
  const PriceDomain domain{Price{-7}, 4U};

  EXPECT_FALSE(domain.contains(Price{-8}));
  EXPECT_TRUE(domain.contains(Price{-7}));
  EXPECT_TRUE(domain.contains(Price{-4}));
  EXPECT_FALSE(domain.contains(Price{-3}));
  EXPECT_EQ(domain.index_of(Price{-7}), 0U);
  EXPECT_EQ(domain.index_of(Price{-4}), 3U);
  EXPECT_EQ(domain.index_of(Price{-8}), std::nullopt);
  EXPECT_EQ(domain.price_at(0U), Price{-7});
  EXPECT_EQ(domain.price_at(3U), Price{-4});
  EXPECT_EQ(domain.price_at(4U), std::nullopt);
}

TEST(PriceDomainTest, AcceptsLargestNonOverflowingDomain) {
  constexpr std::uint32_t count = std::numeric_limits<std::uint32_t>::max();
  const Price minimum{std::numeric_limits<std::int64_t>::max() -
                      static_cast<std::int64_t>(count - 1U)};
  const PriceDomain domain{minimum, count};

  EXPECT_EQ(domain.price_at(count - 1U), Price{std::numeric_limits<std::int64_t>::max()});
}

TEST(HierarchicalBitmapTest, RejectsZeroBits) {
  EXPECT_THROW((HierarchicalBitmap{0U}), std::invalid_argument);
}

TEST(HierarchicalBitmapTest, HandlesBoundariesAndIdempotence) {
  constexpr std::array sizes{1U, 63U, 64U, 65U, 4095U, 4096U, 4097U, 262144U};

  for (const std::uint32_t size : sizes) {
    SCOPED_TRACE(size);
    HierarchicalBitmap bitmap{size};
    const std::uint32_t last = size - 1U;

    EXPECT_EQ(bitmap.first_set(), std::nullopt);
    EXPECT_EQ(bitmap.last_set(), std::nullopt);
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_TRUE(bitmap.set(0U));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_TRUE(bitmap.set(last));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_TRUE(bitmap.set(last));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_EQ(bitmap.test(0U), true);
    EXPECT_EQ(bitmap.test(last), true);
    EXPECT_EQ(bitmap.first_set(), 0U);
    EXPECT_EQ(bitmap.last_set(), last);
    EXPECT_EQ(bitmap.next_set(0U), 0U);
    EXPECT_EQ(bitmap.next_set(last), last);
    EXPECT_EQ(bitmap.previous_set(0U), 0U);
    EXPECT_EQ(bitmap.previous_set(last), last);

    EXPECT_TRUE(bitmap.clear(0U));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_TRUE(bitmap.clear(0U));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_EQ(bitmap.test(0U), false);
    EXPECT_EQ(bitmap.first_set(), last == 0U ? std::nullopt : std::optional<std::uint32_t>{last});
    EXPECT_TRUE(bitmap.clear(last));
    EXPECT_TRUE(bitmap.hierarchy_consistent());
    EXPECT_EQ(bitmap.first_set(), std::nullopt);
  }
}

TEST(HierarchicalBitmapTest, TraversesAcrossHierarchyWordBoundaries) {
  HierarchicalBitmap bitmap{4097U};
  for (const std::uint32_t index : {0U, 63U, 64U, 4095U, 4096U}) {
    ASSERT_TRUE(bitmap.set(index));
  }

  EXPECT_EQ(bitmap.next_set(1U), 63U);
  EXPECT_EQ(bitmap.next_set(63U), 63U);
  EXPECT_EQ(bitmap.next_set(65U), 4095U);
  EXPECT_EQ(bitmap.previous_set(4094U), 64U);
  EXPECT_EQ(bitmap.previous_set(4095U), 4095U);
  EXPECT_EQ(bitmap.previous_set(4096U), 4096U);
}

TEST(HierarchicalBitmapTest, RejectsOutOfRangeOperations) {
  HierarchicalBitmap bitmap{65U};

  EXPECT_FALSE(bitmap.set(65U));
  EXPECT_FALSE(bitmap.clear(65U));
  EXPECT_EQ(bitmap.test(65U), std::nullopt);
  EXPECT_EQ(bitmap.next_set(65U), std::nullopt);
  EXPECT_EQ(bitmap.previous_set(65U), std::nullopt);
  EXPECT_EQ(bitmap.first_set(), std::nullopt);
  EXPECT_EQ(bitmap.last_set(), std::nullopt);
}

std::optional<std::uint32_t> model_first(const std::set<std::uint32_t>& model) {
  return model.empty() ? std::nullopt : std::optional{*model.begin()};
}

std::optional<std::uint32_t> model_last(const std::set<std::uint32_t>& model) {
  return model.empty() ? std::nullopt : std::optional{*model.rbegin()};
}

std::optional<std::uint32_t> model_next(const std::set<std::uint32_t>& model, std::uint32_t index) {
  const auto found = model.lower_bound(index);
  return found == model.end() ? std::nullopt : std::optional{*found};
}

std::optional<std::uint32_t> model_previous(const std::set<std::uint32_t>& model,
                                            std::uint32_t index) {
  const auto found = model.upper_bound(index);
  return found == model.begin() ? std::nullopt : std::optional{*std::prev(found)};
}

TEST(HierarchicalBitmapTest, MatchesDeterministicReferenceModel) {
  constexpr std::uint32_t seed = 0x5EED1234U;
  constexpr std::array sizes{1U, 63U, 64U, 65U, 4095U, 4096U, 4097U, 262144U};
  std::cout << "HierarchicalBitmap differential seed: " << seed << '\n';
  std::mt19937 random{seed}; // NOLINT(bugprone-random-generator-seed): reproducible test

  for (const std::uint32_t size : sizes) {
    SCOPED_TRACE(size);
    HierarchicalBitmap bitmap{size};
    std::set<std::uint32_t> model;

    for (std::uint32_t step = 0; step < 4000U; ++step) {
      const std::uint32_t index = random() % (size + 3U);
      switch (random() % 5U) {
      case 0U:
        EXPECT_EQ(bitmap.set(index), index < size);
        if (index < size) {
          model.insert(index);
        }
        break;
      case 1U:
        EXPECT_EQ(bitmap.clear(index), index < size);
        if (index < size) {
          model.erase(index);
        }
        break;
      case 2U:
        EXPECT_EQ(bitmap.test(index),
                  index < size ? std::optional{model.contains(index)} : std::nullopt);
        break;
      case 3U:
        EXPECT_EQ(bitmap.next_set(index), index < size ? model_next(model, index) : std::nullopt);
        break;
      case 4U:
        EXPECT_EQ(bitmap.previous_set(index),
                  index < size ? model_previous(model, index) : std::nullopt);
        break;
      default:
        FAIL() << "unreachable operation";
      }
      EXPECT_EQ(bitmap.first_set(), model_first(model));
      EXPECT_EQ(bitmap.last_set(), model_last(model));
      EXPECT_TRUE(bitmap.hierarchy_consistent());
    }
  }
}

} // namespace
} // namespace matching_engine
