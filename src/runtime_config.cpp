#include "matching_engine/runtime_config.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <string>

namespace matching_engine {
namespace {

constexpr std::size_t kFieldCount = 12U;

[[nodiscard]] RuntimeConfigField field_from_name(std::string_view name) noexcept {
  if (name == "minimum-price") return RuntimeConfigField::minimum_price;
  if (name == "tick-count") return RuntimeConfigField::tick_count;
  if (name == "max-orders") return RuntimeConfigField::max_orders;
  if (name == "max-quantity") return RuntimeConfigField::max_quantity;
  if (name == "command-queue-capacity") return RuntimeConfigField::command_queue_capacity;
  if (name == "event-queue-capacity") return RuntimeConfigField::event_queue_capacity;
  if (name == "journal-segment-capacity") return RuntimeConfigField::journal_segment_capacity;
  if (name == "journal-prefix") return RuntimeConfigField::journal_prefix;
  if (name == "snapshot-path") return RuntimeConfigField::snapshot_path;
  if (name == "max-lanes") return RuntimeConfigField::max_lanes;
  if (name == "max-notional") return RuntimeConfigField::max_notional;
  if (name == "max-orders-per-second") return RuntimeConfigField::max_orders_per_second;
  return RuntimeConfigField::none;
}

[[nodiscard]] constexpr std::size_t field_index(RuntimeConfigField field) noexcept {
  return static_cast<std::size_t>(field) - 1U;
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& output) noexcept {
  if (text.empty() || text.front() == '+' || text.front() == '-' ||
      (text.size() > 1U && text.front() == '0')) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output, 10);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_signed(std::string_view text, std::int64_t& output) noexcept {
  if (text.empty() || text.front() == '+' || text == "-0" ||
      (text.size() > 1U && text.front() == '0') ||
      (text.size() > 2U && text.starts_with("-0"))) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output, 10);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool valid_path(std::string_view value) noexcept {
  return !value.empty() && value.back() != '/' && value.find('\0') == std::string_view::npos &&
         value != "." && value != "..";
}

} // namespace

GatewayConfig RuntimeConfig::gateway_config() const noexcept {
  const Price maximum{minimum_price.ticks() + static_cast<std::int64_t>(tick_count - 1U)};
  return {.max_active_orders = max_orders,
          .max_lanes = max_lanes,
          .max_quantity = max_quantity,
          .max_notional = max_notional,
          .min_price = minimum_price,
          .max_price = maximum,
          .max_orders_per_second = max_orders_per_second};
}

std::expected<RuntimeConfig, RuntimeConfigError>
parse_runtime_config(std::span<const std::string_view> entries) noexcept {
  RuntimeConfig config;
  std::array<bool, kFieldCount> seen{};
  try {
    for (const std::string_view entry : entries) {
      const std::size_t separator = entry.find('=');
      if (separator == std::string_view::npos || separator == 0U ||
          separator + 1U == entry.size() || entry.find('=', separator + 1U) != std::string_view::npos) {
        return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::malformed_entry,
                                                  RuntimeConfigField::none}};
      }
      const RuntimeConfigField field = field_from_name(entry.substr(0U, separator));
      if (field == RuntimeConfigField::none) {
        return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::unknown_field,
                                                  RuntimeConfigField::none}};
      }
      if (seen[field_index(field)]) {
        return std::unexpected{
            RuntimeConfigError{RuntimeConfigErrorCode::duplicate_field, field}};
      }
      seen[field_index(field)] = true;
      const std::string_view value = entry.substr(separator + 1U);
      std::uint64_t unsigned_value{};
      std::int64_t signed_value{};
      switch (field) {
      case RuntimeConfigField::minimum_price:
        if (!parse_signed(value, signed_value)) {
          return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
        }
        config.minimum_price = Price{signed_value};
        break;
      case RuntimeConfigField::journal_prefix:
        if (!valid_path(value)) {
          return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
        }
        config.journal_prefix = std::string{value};
        break;
      case RuntimeConfigField::snapshot_path:
        if (!valid_path(value)) {
          return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
        }
        config.snapshot_path = std::string{value};
        break;
      default:
        if (!parse_integer(value, unsigned_value)) {
          return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
        }
        if (field == RuntimeConfigField::tick_count) {
          if (unsigned_value > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
          }
          config.tick_count = static_cast<std::uint32_t>(unsigned_value);
        }
        if (field == RuntimeConfigField::max_orders) {
          if (unsigned_value > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
          }
          config.max_orders = static_cast<std::size_t>(unsigned_value);
        }
        if (field == RuntimeConfigField::max_quantity) config.max_quantity = Quantity{unsigned_value};
        if (field == RuntimeConfigField::command_queue_capacity ||
            field == RuntimeConfigField::event_queue_capacity ||
            field == RuntimeConfigField::max_lanes) {
          if (unsigned_value > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value, field}};
          }
        }
        if (field == RuntimeConfigField::command_queue_capacity) config.command_queue_capacity = static_cast<std::size_t>(unsigned_value);
        if (field == RuntimeConfigField::event_queue_capacity) config.event_queue_capacity = static_cast<std::size_t>(unsigned_value);
        if (field == RuntimeConfigField::journal_segment_capacity) config.journal_segment_capacity = unsigned_value;
        if (field == RuntimeConfigField::max_lanes) config.max_lanes = static_cast<std::size_t>(unsigned_value);
        if (field == RuntimeConfigField::max_notional) config.max_notional = unsigned_value;
        if (field == RuntimeConfigField::max_orders_per_second) config.max_orders_per_second = unsigned_value;
        break;
      }
    }
  } catch (...) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::none}};
  }

  for (std::size_t index = 0U; index < seen.size(); ++index) {
    if (!seen[index]) {
      return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::missing_field,
                                                static_cast<RuntimeConfigField>(index + 1U)}};
    }
  }
  if (config.tick_count == 0U || config.tick_count > kMaximumPriceLevels ||
      config.minimum_price.ticks() > std::numeric_limits<std::int64_t>::max() -
                                         static_cast<std::int64_t>(config.tick_count - 1U)) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::tick_count}};
  }
  if (config.max_orders == 0U || config.max_orders > kMaximumSnapshotSlots) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::max_orders}};
  }
  if (config.max_quantity.value() == 0U ||
      config.max_quantity.value() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::max_quantity}};
  }
  if (config.command_queue_capacity == 0U ||
      config.command_queue_capacity > kMaximumSpscCapacity) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::command_queue_capacity}};
  }
  const std::uint64_t required_events = static_cast<std::uint64_t>(config.max_orders) * 2U + 1U;
  if (config.event_queue_capacity > kMaximumSpscCapacity ||
      config.event_queue_capacity < required_events) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_relationship,
                                              RuntimeConfigField::event_queue_capacity}};
  }
  if (config.journal_segment_capacity == 0U ||
      config.journal_segment_capacity > kMaximumJournalCapacity) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              RuntimeConfigField::journal_segment_capacity}};
  }
  if (config.journal_prefix.empty() || config.snapshot_path.empty()) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              config.journal_prefix.empty()
                                                  ? RuntimeConfigField::journal_prefix
                                                  : RuntimeConfigField::snapshot_path}};
  }
  if (config.max_lanes == 0U || config.max_lanes > config.command_queue_capacity) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_relationship,
                                              RuntimeConfigField::max_lanes}};
  }
  if (config.max_notional == 0U || config.max_orders_per_second == 0U) {
    return std::unexpected{RuntimeConfigError{RuntimeConfigErrorCode::invalid_value,
                                              config.max_notional == 0U
                                                  ? RuntimeConfigField::max_notional
                                                  : RuntimeConfigField::max_orders_per_second}};
  }
  return config;
}

const char* runtime_config_field_name(RuntimeConfigField field) noexcept {
  switch (field) {
  case RuntimeConfigField::none: return "none";
  case RuntimeConfigField::minimum_price: return "minimum-price";
  case RuntimeConfigField::tick_count: return "tick-count";
  case RuntimeConfigField::max_orders: return "max-orders";
  case RuntimeConfigField::max_quantity: return "max-quantity";
  case RuntimeConfigField::command_queue_capacity: return "command-queue-capacity";
  case RuntimeConfigField::event_queue_capacity: return "event-queue-capacity";
  case RuntimeConfigField::journal_segment_capacity: return "journal-segment-capacity";
  case RuntimeConfigField::journal_prefix: return "journal-prefix";
  case RuntimeConfigField::snapshot_path: return "snapshot-path";
  case RuntimeConfigField::max_lanes: return "max-lanes";
  case RuntimeConfigField::max_notional: return "max-notional";
  case RuntimeConfigField::max_orders_per_second: return "max-orders-per-second";
  }
  return "unknown";
}

const char* runtime_config_error_message(RuntimeConfigErrorCode code) noexcept {
  switch (code) {
  case RuntimeConfigErrorCode::none: return "none";
  case RuntimeConfigErrorCode::malformed_entry: return "malformed configuration entry";
  case RuntimeConfigErrorCode::unknown_field: return "unknown configuration field";
  case RuntimeConfigErrorCode::duplicate_field: return "duplicate configuration field";
  case RuntimeConfigErrorCode::missing_field: return "missing configuration field";
  case RuntimeConfigErrorCode::invalid_value: return "invalid configuration value";
  case RuntimeConfigErrorCode::invalid_relationship: return "invalid configuration relationship";
  }
  return "unknown configuration error";
}

} // namespace matching_engine
