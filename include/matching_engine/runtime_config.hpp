#ifndef MATCHING_ENGINE_RUNTIME_CONFIG_HPP
#define MATCHING_ENGINE_RUNTIME_CONFIG_HPP

#include "matching_engine/gateway.hpp"
#include "matching_engine/journal.hpp"
#include "matching_engine/order_book.hpp"
#include "matching_engine/snapshot.hpp"
#include "matching_engine/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>

namespace matching_engine {

inline constexpr std::uint32_t kRuntimeApiVersionMajor = 1U;
inline constexpr std::uint32_t kRuntimeApiVersionMinor = 0U;

enum class RuntimeConfigField : std::uint8_t {
  none,
  minimum_price,
  tick_count,
  max_orders,
  max_quantity,
  command_queue_capacity,
  event_queue_capacity,
  journal_segment_capacity,
  journal_prefix,
  snapshot_path,
  max_lanes,
  max_notional,
  max_orders_per_second,
};

enum class RuntimeConfigErrorCode : std::uint8_t {
  none,
  malformed_entry,
  unknown_field,
  duplicate_field,
  missing_field,
  invalid_value,
  invalid_relationship,
};

struct RuntimeConfigError {
  RuntimeConfigErrorCode code{RuntimeConfigErrorCode::none};
  RuntimeConfigField field{RuntimeConfigField::none};

  constexpr bool operator==(const RuntimeConfigError&) const noexcept = default;
};

struct RuntimeConfig {
  Price minimum_price{0};
  std::uint32_t tick_count{};
  std::size_t max_orders{};
  Quantity max_quantity{0U};
  std::size_t command_queue_capacity{};
  std::size_t event_queue_capacity{};
  std::uint64_t journal_segment_capacity{};
  std::filesystem::path journal_prefix;
  std::filesystem::path snapshot_path;
  std::size_t max_lanes{};
  std::uint64_t max_notional{};
  std::uint64_t max_orders_per_second{};

  [[nodiscard]] GatewayConfig gateway_config() const noexcept;
};

[[nodiscard]] constexpr bool runtime_api_compatible(std::uint32_t major,
                                                     std::uint32_t minor) noexcept {
  return major == kRuntimeApiVersionMajor && minor <= kRuntimeApiVersionMinor;
}

[[nodiscard]] std::expected<RuntimeConfig, RuntimeConfigError>
parse_runtime_config(std::span<const std::string_view> entries) noexcept;
[[nodiscard]] const char* runtime_config_field_name(RuntimeConfigField field) noexcept;
[[nodiscard]] const char* runtime_config_error_message(RuntimeConfigErrorCode code) noexcept;

} // namespace matching_engine

#endif
