#include "matching_engine/gateway.hpp"

#include <algorithm>
#include <limits>

namespace matching_engine {

GatewayValidator::GatewayValidator(GatewayConfig config) noexcept : config_{config} {
  if (config_.max_orders_per_second == 0U) {
    config_.max_orders_per_second = 1U;
  }
  if (config_.max_lanes == 0U) {
    config_.max_lanes = 1U;
  }
  active_order_ids_.reserve(config_.max_active_orders);
  rate_windows_.resize(config_.max_lanes);
}

bool GatewayValidator::rate_limit_exceeded(LaneId lane_id, std::uint64_t logical_time) noexcept {
  if (config_.max_orders_per_second == 0U) {
    return false;
  }

  constexpr std::uint64_t kRateWindowSize = 1'000ULL;
  const std::uint64_t bucket_start = logical_time / kRateWindowSize;
  RateWindow& window = rate_windows_[lane_id];

  if (window.count == 0U || bucket_start != window.start) {
    window.start = bucket_start;
    window.count = 0U;
  }

  if (window.count >= config_.max_orders_per_second) {
    return true;
  }

  ++window.count;
  return false;
}

std::uint64_t GatewayValidator::absolute_notional(Price price, Quantity quantity) const noexcept {
  const auto ticks = price.ticks();
  const auto price_ticks = ticks < 0 ? static_cast<std::uint64_t>(-(ticks + 1)) + 1U
                                     : static_cast<std::uint64_t>(ticks);
  const auto quantity_value = quantity.value();
  if (price_ticks != 0U && quantity_value > std::numeric_limits<std::uint64_t>::max() / price_ticks) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return price_ticks * quantity_value;
}

GatewayRejectReason GatewayValidator::validate(OrderId id, Side side, Price price,
                                              Quantity quantity, std::uint64_t logical_time) noexcept {
  return validate(LaneId{0U}, id, side, price, quantity, logical_time);
}

GatewayRejectReason GatewayValidator::validate(LaneId lane_id, OrderId id, Side side, Price price,
                                              Quantity quantity, std::uint64_t logical_time) noexcept {
  (void)side;

  if (lane_id >= config_.max_lanes) {
    return GatewayRejectReason::invalid_lane;
  }

  if (id.value() == 0U) {
    return GatewayRejectReason::duplicate_order_id;
  }

  if (std::find(active_order_ids_.begin(), active_order_ids_.end(), id.value()) !=
      active_order_ids_.end()) {
    return GatewayRejectReason::duplicate_order_id;
  }

  if (active_order_ids_.size() >= config_.max_active_orders) {
    return GatewayRejectReason::order_capacity_exhausted;
  }

  if (quantity > config_.max_quantity) {
    return GatewayRejectReason::quantity_too_large;
  }

  if (price < config_.min_price || price > config_.max_price) {
    return GatewayRejectReason::price_out_of_collar;
  }

  if (absolute_notional(price, quantity) > config_.max_notional) {
    return GatewayRejectReason::notional_too_large;
  }

  if (rate_limit_exceeded(lane_id, logical_time)) {
    return GatewayRejectReason::rate_limited;
  }

  active_order_ids_.push_back(id.value());
  return GatewayRejectReason::none;
}

void GatewayValidator::release(OrderId id) noexcept {
  const auto iterator = std::find(active_order_ids_.begin(), active_order_ids_.end(), id.value());
  if (iterator != active_order_ids_.end()) {
    *iterator = active_order_ids_.back();
    active_order_ids_.pop_back();
  }
}

} // namespace matching_engine