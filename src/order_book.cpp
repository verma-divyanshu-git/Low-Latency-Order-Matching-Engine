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
      arena_{max_orders},
      visit_marks_{arena_.capacity() == 0U ? nullptr
                                           : std::make_unique<std::uint32_t[]>(arena_.capacity())} {
}

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

InvariantResult OrderBook::invariant_failure(InvariantViolation violation, Side side,
                                             std::uint32_t level_index, std::uint32_t order_index,
                                             std::uint32_t reachable_count) noexcept {
  return {.violation = violation,
          .side = side,
          .level_index = level_index,
          .order_index = order_index,
          .reachable_count = reachable_count};
}

InvariantResult OrderBook::check_side_invariants(Side side, std::uint32_t epoch,
                                                 std::uint32_t& reachable_count) noexcept {
  for (std::uint32_t level_index = 0U; level_index < domain_.tick_count(); ++level_index) {
    const PriceLevel& price_level = level(side, level_index);
    const bool nonempty = price_level.order_count != 0U;
    const bool occupied = occupancy(side).test(level_index).value_or(false);
    if (occupied != nonempty) {
      return invariant_failure(InvariantViolation::occupancy_mismatch, side, level_index,
                               kInvalidIndex, reachable_count);
    }
    if (!nonempty) {
      if (price_level.head_index != kInvalidIndex || price_level.tail_index != kInvalidIndex ||
          price_level.aggregate_quantity != 0U) {
        return invariant_failure(InvariantViolation::empty_level_metadata, side, level_index,
                                 kInvalidIndex, reachable_count);
      }
      continue;
    }

    if (price_level.head_index == kInvalidIndex) {
      return invariant_failure(InvariantViolation::nonempty_invalid_head, side, level_index,
                               kInvalidIndex, reachable_count);
    }
    if (price_level.tail_index == kInvalidIndex) {
      return invariant_failure(InvariantViolation::nonempty_invalid_tail, side, level_index,
                               kInvalidIndex, reachable_count);
    }
    if (price_level.head_index >= arena_.capacity()) {
      return invariant_failure(InvariantViolation::order_index_out_of_range, side, level_index,
                               price_level.head_index, reachable_count);
    }
    if (!arena_.is_live_index(price_level.head_index)) {
      return invariant_failure(InvariantViolation::dead_order_reachable, side, level_index,
                               price_level.head_index, reachable_count);
    }
    if (price_level.tail_index >= arena_.capacity()) {
      return invariant_failure(InvariantViolation::order_index_out_of_range, side, level_index,
                               price_level.tail_index, reachable_count);
    }
    if (!arena_.is_live_index(price_level.tail_index)) {
      return invariant_failure(InvariantViolation::dead_order_reachable, side, level_index,
                               price_level.tail_index, reachable_count);
    }
    if (arena_.order_at(price_level.head_index).prev_index != kInvalidIndex) {
      return invariant_failure(InvariantViolation::head_prev_not_invalid, side, level_index,
                               price_level.head_index, reachable_count);
    }
    if (arena_.order_at(price_level.tail_index).next_index != kInvalidIndex) {
      return invariant_failure(InvariantViolation::tail_next_not_invalid, side, level_index,
                               price_level.tail_index, reachable_count);
    }

    std::uint32_t current = price_level.head_index;
    std::uint32_t previous = kInvalidIndex;
    std::uint32_t walked_count = 0U;
    std::uint64_t walked_quantity = 0U;
    while (current != kInvalidIndex) {
      if (current >= arena_.capacity()) {
        return invariant_failure(InvariantViolation::order_index_out_of_range, side, level_index,
                                 current, reachable_count);
      }
      if (!arena_.is_live_index(current)) {
        return invariant_failure(InvariantViolation::dead_order_reachable, side, level_index,
                                 current, reachable_count);
      }
      if (visit_marks_[current] == epoch) {
        return invariant_failure(InvariantViolation::duplicate_order_reachable, side, level_index,
                                 current, reachable_count);
      }

      const Order& order = arena_.order_at(current);
      if (order.prev_index != previous) {
        return invariant_failure(InvariantViolation::previous_not_reciprocal, side, level_index,
                                 current, reachable_count);
      }
      if (detail::decode_side(order.encoded_level_side) != side) {
        return invariant_failure(InvariantViolation::order_side_mismatch, side, level_index,
                                 current, reachable_count);
      }
      if (detail::decode_level(order.encoded_level_side) != level_index) {
        return invariant_failure(InvariantViolation::order_level_mismatch, side, level_index,
                                 current, reachable_count);
      }
      if (order.remaining.value() == 0U) {
        return invariant_failure(InvariantViolation::nonpositive_remaining, side, level_index,
                                 current, reachable_count);
      }
      if (walked_quantity > std::numeric_limits<std::uint64_t>::max() - order.remaining.value()) {
        return invariant_failure(InvariantViolation::aggregate_overflow, side, level_index, current,
                                 reachable_count);
      }

      visit_marks_[current] = epoch;
      ++reachable_count;
      ++walked_count;
      walked_quantity += order.remaining.value();
      const std::uint32_t next = order.next_index;
      if (next != kInvalidIndex) {
        if (next >= arena_.capacity()) {
          return invariant_failure(InvariantViolation::order_index_out_of_range, side, level_index,
                                   next, reachable_count);
        }
        if (!arena_.is_live_index(next)) {
          return invariant_failure(InvariantViolation::dead_order_reachable, side, level_index,
                                   next, reachable_count);
        }
        if (arena_.order_at(next).prev_index != current) {
          return invariant_failure(InvariantViolation::next_not_reciprocal, side, level_index,
                                   current, reachable_count);
        }
      }
      previous = current;
      current = next;
    }

    if (previous != price_level.tail_index) {
      return invariant_failure(InvariantViolation::level_tail_mismatch, side, level_index, previous,
                               reachable_count);
    }
    if (walked_count != price_level.order_count) {
      return invariant_failure(InvariantViolation::level_count_mismatch, side, level_index,
                               kInvalidIndex, reachable_count);
    }
    if (walked_quantity != price_level.aggregate_quantity) {
      return invariant_failure(InvariantViolation::level_aggregate_mismatch, side, level_index,
                               kInvalidIndex, reachable_count);
    }
  }
  return {.reachable_count = reachable_count};
}

InvariantResult OrderBook::check_invariants() noexcept {
  if (visit_epoch_ == std::numeric_limits<std::uint32_t>::max()) {
    for (std::uint32_t index = 0U; index < arena_.capacity(); ++index) {
      visit_marks_[index] = 0U;
    }
    visit_epoch_ = 1U;
  } else {
    ++visit_epoch_;
  }

  std::uint32_t reachable_count = 0U;
  InvariantResult result = check_side_invariants(Side::buy, visit_epoch_, reachable_count);
  if (result.violation != InvariantViolation::none) {
    return result;
  }
  result = check_side_invariants(Side::sell, visit_epoch_, reachable_count);
  if (result.violation != InvariantViolation::none) {
    return result;
  }

  for (std::uint32_t index = 0U; index < arena_.capacity(); ++index) {
    if (!arena_.is_live_index(index)) {
      continue;
    }
    if (arena_.order_at(index).remaining.value() == 0U) {
      return invariant_failure(InvariantViolation::nonpositive_remaining,
                               detail::decode_side(arena_.order_at(index).encoded_level_side),
                               detail::decode_level(arena_.order_at(index).encoded_level_side),
                               index, reachable_count);
    }
    if (visit_marks_[index] != visit_epoch_) {
      return invariant_failure(InvariantViolation::live_order_unreachable,
                               detail::decode_side(arena_.order_at(index).encoded_level_side),
                               detail::decode_level(arena_.order_at(index).encoded_level_side),
                               index, reachable_count);
    }
  }
  if (reachable_count != arena_.size()) {
    return invariant_failure(InvariantViolation::reachable_count_mismatch, Side::buy, kInvalidIndex,
                             kInvalidIndex, reachable_count);
  }

  const auto bid = best_index(Side::buy);
  const auto ask = best_index(Side::sell);
  if (bid.has_value() && ask.has_value() && *bid >= *ask) {
    return invariant_failure(InvariantViolation::crossed_book, Side::buy, *bid, kInvalidIndex,
                             reachable_count);
  }
  return {.reachable_count = reachable_count};
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
