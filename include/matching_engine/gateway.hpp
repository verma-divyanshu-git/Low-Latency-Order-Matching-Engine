#ifndef MATCHING_ENGINE_GATEWAY_HPP
#define MATCHING_ENGINE_GATEWAY_HPP

#include "matching_engine/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace matching_engine {

struct GatewayConfig {
  std::size_t max_active_orders{0U};
  Quantity max_quantity{Quantity{0U}};
  std::uint64_t max_notional{0U};
  Price min_price{Price{0}};
  Price max_price{Price{0}};
  std::uint64_t max_orders_per_second{0U};
};

enum class GatewayRejectReason : std::uint8_t {
  none,
  duplicate_order_id,
  order_capacity_exhausted,
  quantity_too_large,
  price_out_of_collar,
  rate_limited,
  notional_too_large,
};

class GatewayValidator {
public:
  explicit GatewayValidator(GatewayConfig config) noexcept;

  [[nodiscard]] GatewayRejectReason validate(OrderId id, Side side, Price price,
                                             Quantity quantity,
                                             std::uint64_t logical_time) noexcept;

  void release(OrderId id) noexcept;

private:
  [[nodiscard]] bool rate_limit_exceeded(std::uint64_t logical_time) noexcept;
  [[nodiscard]] std::uint64_t absolute_notional(Price price, Quantity quantity) const noexcept;

  GatewayConfig config_{};
  std::vector<std::uint64_t> active_order_ids_{};
  std::uint64_t current_rate_window_start_{};
  std::uint64_t current_rate_window_count_{0U};
};

} // namespace matching_engine

#endif