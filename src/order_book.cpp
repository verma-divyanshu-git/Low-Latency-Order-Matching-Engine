#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace matching_engine {
namespace {

constexpr Handle kInvalidHandle{.index = kInvalidIndex, .generation = 0U};

[[nodiscard]] SubmitResult rejected(RejectReason reason, Quantity quantity) noexcept {
  return {.reject_reason = reason,
          .executed_quantity = Quantity{0U},
          .unfilled_quantity = quantity,
          .trade_count = 0U,
          .resting_handle = kInvalidHandle};
}

[[nodiscard]] constexpr Side opposite_side(Side side) noexcept {
  switch (side) {
  case Side::buy:
    return Side::sell;
  case Side::sell:
    return Side::buy;
  }
  std::unreachable();
}

[[nodiscard]] constexpr bool crosses(Side taker_side, std::uint32_t maker_level,
                                     std::uint32_t limit_level) noexcept {
  switch (taker_side) {
  case Side::buy:
    return maker_level <= limit_level;
  case Side::sell:
    return maker_level >= limit_level;
  }
  std::unreachable();
}

[[nodiscard]] constexpr bool is_valid_time_in_force(TimeInForce time_in_force) noexcept {
  return time_in_force == TimeInForce::gtc || time_in_force == TimeInForce::ioc ||
         time_in_force == TimeInForce::fok;
}

[[nodiscard]] Trade make_trade(Side taker_side, OrderId taker_id, OrderId maker_id, Price price,
                               Quantity quantity) noexcept {
  switch (taker_side) {
  case Side::buy:
    return {taker_id, maker_id, price, quantity};
  case Side::sell:
    return {maker_id, taker_id, price, quantity};
  }
  std::unreachable();
}

} // namespace

OrderBook::OrderBook(PriceDomain domain, std::size_t max_orders, Quantity max_order_quantity)
    : domain_{checked_domain(domain)},
      max_order_quantity_{checked_max_quantity(max_order_quantity)},
      bids_{std::make_unique<PriceLevel[]>(domain_.tick_count())},
      asks_{std::make_unique<PriceLevel[]>(domain_.tick_count())},
      bid_occupancy_{domain_.tick_count()}, ask_occupancy_{domain_.tick_count()},
      arena_{max_orders} {}

PriceDomain OrderBook::checked_domain(PriceDomain domain) {
  if (!detail::is_encodable_tick_count(domain.tick_count())) {
    throw std::length_error{"OrderBook price domain exceeds encoded level range"};
  }
  return domain;
}

std::uint32_t OrderBook::checked_max_quantity(Quantity quantity) {
  if (quantity.value() == 0U || quantity.value() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument{"OrderBook maximum order quantity must fit uint32 and be positive"};
  }
  return static_cast<std::uint32_t>(quantity.value());
}

SubmitResult OrderBook::validate(Side side, Quantity quantity,
                                 std::span<Trade> trades) const noexcept {
  if (!is_valid_side(side)) {
    return rejected(RejectReason::invalid_side, quantity);
  }
  if (quantity.value() == 0U) {
    return rejected(RejectReason::zero_quantity, quantity);
  }
  if (quantity.value() > max_order_quantity_) {
    return rejected(RejectReason::quantity_too_large, quantity);
  }
  if (trades.size() < required_trade_capacity()) {
    return rejected(RejectReason::insufficient_trade_capacity, quantity);
  }
  return rejected(RejectReason::none, quantity);
}

SubmitResult OrderBook::submit_limit(OrderId id, Side side, Price price, Quantity quantity,
                                     std::span<Trade> trades) noexcept {
  return submit_limit(id, side, price, quantity, TimeInForce::gtc, trades);
}

SubmitResult OrderBook::submit_limit(OrderId id, Side side, Price price, Quantity quantity,
                                     TimeInForce time_in_force, std::span<Trade> trades) noexcept {
  if (!is_valid_time_in_force(time_in_force)) {
    return rejected(RejectReason::invalid_time_in_force, quantity);
  }
  const SubmitResult validation = validate(side, quantity, trades);
  if (validation.reject_reason != RejectReason::none) {
    return validation;
  }
  const auto level_index = domain_.index_of(price);
  if (!level_index.has_value()) {
    return rejected(RejectReason::price_out_of_domain, quantity);
  }
  if (time_in_force == TimeInForce::fok && !can_fully_fill(side, *level_index, quantity.value())) {
    return rejected(RejectReason::fok_not_fillable, quantity);
  }
  if (time_in_force == TimeInForce::gtc && arena_.size() == arena_.capacity() &&
      !has_crossing_order(side, *level_index)) {
    return rejected(RejectReason::order_capacity_exhausted, quantity);
  }

  std::uint64_t remaining = quantity.value();
  std::uint32_t trade_count = 0U;
  match(id, side, level_index, remaining, trades, trade_count);
  const bool should_rest = time_in_force == TimeInForce::gtc && remaining != 0U;
  const Handle handle = should_rest ? rest(id, side, *level_index, remaining) : kInvalidHandle;
  return {.reject_reason = RejectReason::none,
          .executed_quantity = Quantity{quantity.value() - remaining},
          .unfilled_quantity = Quantity{remaining},
          .trade_count = trade_count,
          .resting_handle = handle};
}

bool OrderBook::can_fully_fill(Side side, std::uint32_t limit_index,
                               std::uint64_t quantity) const noexcept {
  const Side maker_side = opposite_side(side);
  auto maker_level = best_index(maker_side);
  std::uint64_t available = 0U;
  while (maker_level.has_value() && crosses(side, *maker_level, limit_index)) {
    const std::uint64_t level_quantity = level(maker_side, *maker_level).aggregate_quantity;
    const std::uint64_t needed = quantity - available;
    available += std::min(level_quantity, needed);
    if (available == quantity) {
      return true;
    }
    maker_level = next_level(maker_side, *maker_level);
  }
  return false;
}

std::optional<std::uint32_t> OrderBook::next_level(Side side,
                                                   std::uint32_t current) const noexcept {
  switch (side) {
  case Side::buy:
    return current == 0U ? std::nullopt : occupancy(side).previous_set(current - 1U);
  case Side::sell:
    return current + 1U >= domain_.tick_count() ? std::nullopt
                                                : occupancy(side).next_set(current + 1U);
  }
  std::unreachable();
}

SubmitResult OrderBook::submit_market(OrderId id, Side side, Quantity quantity,
                                      std::span<Trade> trades) noexcept {
  const SubmitResult validation = validate(side, quantity, trades);
  if (validation.reject_reason != RejectReason::none) {
    return validation;
  }

  std::uint64_t remaining = quantity.value();
  std::uint32_t trade_count = 0U;
  match(id, side, std::nullopt, remaining, trades, trade_count);
  return {.reject_reason = RejectReason::none,
          .executed_quantity = Quantity{quantity.value() - remaining},
          .unfilled_quantity = Quantity{remaining},
          .trade_count = trade_count,
          .resting_handle = kInvalidHandle};
}

bool OrderBook::has_crossing_order(Side side, std::uint32_t limit_index) const noexcept {
  const auto opposite_best = best_index(opposite_side(side));
  return opposite_best.has_value() && crosses(side, *opposite_best, limit_index);
}

void OrderBook::match(OrderId taker_id, Side taker_side, std::optional<std::uint32_t> limit_index,
                      std::uint64_t& remaining, std::span<Trade> trades,
                      std::uint32_t& trade_count) noexcept {
  const Side maker_side = opposite_side(taker_side);
  while (remaining != 0U) {
    const auto maker_level = best_index(maker_side);
    if (!maker_level.has_value()) {
      return;
    }
    if (limit_index.has_value() && !crosses(taker_side, *maker_level, *limit_index)) {
      return;
    }
    match_level(taker_id, taker_side, *maker_level, remaining, trades, trade_count);
  }
}

void OrderBook::match_level(OrderId taker_id, Side taker_side, std::uint32_t level_index,
                            std::uint64_t& remaining, std::span<Trade> trades,
                            std::uint32_t& trade_count) noexcept {
  const Side maker_side = opposite_side(taker_side);
  PriceLevel& price_level = level(maker_side, level_index);
  const auto execution_price = domain_.price_at(level_index);
  if (!execution_price.has_value()) {
    return;
  }
  while (remaining != 0U && price_level.head_index != kInvalidIndex) {
    Order& maker = arena_.order_at(price_level.head_index);
    const std::uint64_t execution = std::min(remaining, maker.remaining.value());
    trades[trade_count++] =
        make_trade(taker_side, taker_id, maker.id, *execution_price, Quantity{execution});
    remaining -= execution;
    price_level.aggregate_quantity -= execution;
    if (execution == maker.remaining.value()) {
      unlink(arena_.handle_at(price_level.head_index), 0U);
    } else {
      maker.remaining = Quantity{maker.remaining.value() - execution};
    }
  }
}

void OrderBook::unlink(Handle handle, std::uint64_t aggregate_reduction) noexcept {
  const Order& removed = arena_.order_at(handle.index);
  const Side side = detail::decode_side(removed.encoded_level_side);
  const std::uint32_t level_index = detail::decode_level(removed.encoded_level_side);
  PriceLevel& price_level = level(side, level_index);
  const std::uint32_t previous_index = removed.prev_index;
  const std::uint32_t next_index = removed.next_index;
  if (previous_index == kInvalidIndex) {
    price_level.head_index = next_index;
  } else {
    arena_.order_at(previous_index).next_index = next_index;
  }
  if (next_index == kInvalidIndex) {
    price_level.tail_index = previous_index;
  } else {
    arena_.order_at(next_index).prev_index = previous_index;
  }
  price_level.aggregate_quantity -= aggregate_reduction;
  --price_level.order_count;
  if (price_level.order_count == 0U) {
    static_cast<void>(occupancy(side).clear(level_index));
  }
  static_cast<void>(arena_.release(handle));
}

CancelResult OrderBook::cancel(Handle handle) noexcept {
  const Order* order = arena_.resolve(handle);
  if (order == nullptr) {
    return {CancelReason::invalid_handle, OrderId{0U}, Quantity{0U}};
  }
  const OrderId id = order->id;
  const Quantity remaining = order->remaining;
  unlink(handle, remaining.value());
  return {CancelReason::none, id, remaining};
}

AmendResult OrderBook::amend_quantity(Handle handle, Quantity new_remaining) noexcept {
  Order* order = arena_.resolve(handle);
  if (order == nullptr) {
    return {AmendReason::invalid_handle, OrderId{0U}, Quantity{0U}, new_remaining, handle};
  }
  const Quantity previous = order->remaining;
  if (new_remaining.value() == 0U) {
    return {AmendReason::zero_quantity, order->id, previous, new_remaining, handle};
  }
  if (new_remaining.value() > max_order_quantity_) {
    return {AmendReason::quantity_too_large, order->id, previous, new_remaining, handle};
  }
  if (new_remaining > previous) {
    return {AmendReason::increase_not_allowed, order->id, previous, new_remaining, handle};
  }
  if (new_remaining == previous) {
    return {AmendReason::none, order->id, previous, new_remaining, handle};
  }
  const Side side = detail::decode_side(order->encoded_level_side);
  const std::uint32_t level_index = detail::decode_level(order->encoded_level_side);
  level(side, level_index).aggregate_quantity -= previous.value() - new_remaining.value();
  order->remaining = new_remaining;
  return {AmendReason::none, order->id, previous, new_remaining, handle};
}

SubmitResult OrderBook::replace(Handle handle, Price new_price, Quantity new_quantity,
                                std::span<Trade> trades) noexcept {
  const Order* order = arena_.resolve(handle);
  if (order == nullptr) {
    return rejected(RejectReason::invalid_handle, new_quantity);
  }
  const Side side = detail::decode_side(order->encoded_level_side);
  const SubmitResult validation = validate(side, new_quantity, trades);
  if (validation.reject_reason != RejectReason::none) {
    return validation;
  }
  if (!domain_.index_of(new_price).has_value()) {
    return rejected(RejectReason::price_out_of_domain, new_quantity);
  }

  const OrderId id = order->id;
  const std::uint64_t old_remaining = order->remaining.value();
  unlink(handle, old_remaining);
  return submit_limit(id, side, new_price, new_quantity, TimeInForce::gtc, trades);
}

Handle OrderBook::rest(OrderId id, Side side, std::uint32_t level_index,
                       std::uint64_t remaining) noexcept {
  PriceLevel& price_level = level(side, level_index);
  const Order order{.id = id,
                    .remaining = Quantity{remaining},
                    .prev_index = price_level.tail_index,
                    .next_index = kInvalidIndex,
                    .encoded_level_side = detail::encode_level_side(level_index, side),
                    .reserved_flags = 0U};
  const AcquireResult acquired = arena_.acquire(order);
  if (price_level.tail_index == kInvalidIndex) {
    price_level.head_index = acquired.handle.index;
    static_cast<void>(occupancy(side).set(level_index));
  } else {
    arena_.order_at(price_level.tail_index).next_index = acquired.handle.index;
  }
  price_level.tail_index = acquired.handle.index;
  price_level.aggregate_quantity += remaining;
  ++price_level.order_count;
  return acquired.handle;
}

std::optional<std::uint32_t> OrderBook::best_index(Side side) const noexcept {
  switch (side) {
  case Side::buy:
    return bid_occupancy_.last_set();
  case Side::sell:
    return ask_occupancy_.first_set();
  }
  std::unreachable();
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  const auto index = best_index(Side::buy);
  return index.has_value() ? domain_.price_at(*index) : std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  const auto index = best_index(Side::sell);
  return index.has_value() ? domain_.price_at(*index) : std::nullopt;
}

std::optional<LevelInfo> OrderBook::level_info(Side side, Price price) const noexcept {
  if (!is_valid_side(side)) {
    return std::nullopt;
  }
  const auto index = domain_.index_of(price);
  if (!index.has_value()) {
    return std::nullopt;
  }
  const PriceLevel& price_level = level(side, *index);
  return LevelInfo{.aggregate_quantity = Quantity{price_level.aggregate_quantity},
                   .order_count = price_level.order_count};
}

std::size_t OrderBook::required_trade_capacity() const noexcept {
  return arena_.capacity();
}

PriceLevel& OrderBook::level(Side side, std::uint32_t index) noexcept {
  switch (side) {
  case Side::buy:
    return bids_[index];
  case Side::sell:
    return asks_[index];
  }
  std::unreachable();
}

const PriceLevel& OrderBook::level(Side side, std::uint32_t index) const noexcept {
  switch (side) {
  case Side::buy:
    return bids_[index];
  case Side::sell:
    return asks_[index];
  }
  std::unreachable();
}

HierarchicalBitmap& OrderBook::occupancy(Side side) noexcept {
  switch (side) {
  case Side::buy:
    return bid_occupancy_;
  case Side::sell:
    return ask_occupancy_;
  }
  std::unreachable();
}

const HierarchicalBitmap& OrderBook::occupancy(Side side) const noexcept {
  switch (side) {
  case Side::buy:
    return bid_occupancy_;
  case Side::sell:
    return ask_occupancy_;
  }
  std::unreachable();
}

} // namespace matching_engine
