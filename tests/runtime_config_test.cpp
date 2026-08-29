#include "matching_engine/runtime_config.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>

namespace matching_engine {
namespace {

constexpr std::array<std::string_view, 12U> kValidConfig{
    "minimum-price=-100", "tick-count=201", "max-orders=8", "max-quantity=1000",
    "command-queue-capacity=16", "event-queue-capacity=17",
    "journal-segment-capacity=1024", "journal-prefix=/tmp/orders",
    "snapshot-path=/tmp/orders.snapshot", "max-lanes=4", "max-notional=1000000",
    "max-orders-per-second=10000"};

TEST(RuntimeConfigTest, ParsesCompleteCanonicalConfiguration) {
  const auto parsed = parse_runtime_config(kValidConfig);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->minimum_price, Price{-100});
  EXPECT_EQ(parsed->tick_count, 201U);
  EXPECT_EQ(parsed->max_orders, 8U);
  EXPECT_EQ(parsed->event_queue_capacity, 17U);
  const GatewayConfig gateway = parsed->gateway_config();
  EXPECT_EQ(gateway.min_price, Price{-100});
  EXPECT_EQ(gateway.max_price, Price{100});
  EXPECT_EQ(gateway.max_lanes, 4U);
}

TEST(RuntimeConfigTest, RejectsMissingDuplicateUnknownAndMalformedEntries) {
  EXPECT_EQ(parse_runtime_config(std::span{kValidConfig}.first(kValidConfig.size() - 1U)).error(),
            (RuntimeConfigError{RuntimeConfigErrorCode::missing_field,
                                RuntimeConfigField::max_orders_per_second}));
  auto duplicate = kValidConfig;
  duplicate.back() = "max-orders=9";
  EXPECT_EQ(parse_runtime_config(duplicate).error().code,
            RuntimeConfigErrorCode::duplicate_field);
  auto unknown = kValidConfig;
  unknown.back() = "secret=1";
  EXPECT_EQ(parse_runtime_config(unknown).error().code, RuntimeConfigErrorCode::unknown_field);
  auto malformed = kValidConfig;
  malformed.back() = "max-orders-per-second==1";
  EXPECT_EQ(parse_runtime_config(malformed).error().code,
            RuntimeConfigErrorCode::malformed_entry);
}

TEST(RuntimeConfigTest, RejectsNoncanonicalAndOverflowingNumbers) {
  for (const std::string_view value : {"max-orders=01", "max-orders=+1", "max-orders=-1",
                                       "max-orders=18446744073709551616"}) {
    auto entries = kValidConfig;
    entries[2U] = value;
    EXPECT_EQ(parse_runtime_config(entries).error().code, RuntimeConfigErrorCode::invalid_value);
  }
  auto narrowing = kValidConfig;
  narrowing[1U] = "tick-count=4294967297";
  EXPECT_EQ(parse_runtime_config(narrowing).error().code, RuntimeConfigErrorCode::invalid_value);
}

TEST(RuntimeConfigTest, RejectsMalformedPersistencePaths) {
  auto trailing = kValidConfig;
  trailing[7U] = "journal-prefix=/tmp/orders/";
  EXPECT_EQ(parse_runtime_config(trailing).error(),
            (RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                RuntimeConfigField::journal_prefix}));
  auto dot = kValidConfig;
  dot[8U] = "snapshot-path=..";
  EXPECT_EQ(parse_runtime_config(dot).error(),
            (RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                RuntimeConfigField::snapshot_path}));
}

TEST(RuntimeConfigTest, RejectsUnsafeCrossFieldResourceSizing) {
  auto event_queue = kValidConfig;
  event_queue[5U] = "event-queue-capacity=16";
  EXPECT_EQ(parse_runtime_config(event_queue).error(),
            (RuntimeConfigError{RuntimeConfigErrorCode::invalid_relationship,
                                RuntimeConfigField::event_queue_capacity}));
  auto lanes = kValidConfig;
  lanes[9U] = "max-lanes=17";
  EXPECT_EQ(parse_runtime_config(lanes).error(),
            (RuntimeConfigError{RuntimeConfigErrorCode::invalid_relationship,
                                RuntimeConfigField::max_lanes}));
}

TEST(RuntimeApiVersionTest, RequiresSameMajorAndSupportedMinor) {
  EXPECT_TRUE(runtime_api_compatible(1U, 0U));
  EXPECT_FALSE(runtime_api_compatible(0U, 0U));
  EXPECT_FALSE(runtime_api_compatible(2U, 0U));
  EXPECT_FALSE(runtime_api_compatible(1U, 1U));
}

} // namespace
} // namespace matching_engine
