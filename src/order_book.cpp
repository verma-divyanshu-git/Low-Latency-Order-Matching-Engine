#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

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

} // namespace

OrderBook::OrderBook(PriceDomain domain, std::size_t max_orders, Quantity max_order_quantity)
    : domain_{checked_domain(domain)},
      max_order_quantity_{checked_max_quantity(max_order_quantity)},
      bids_{std::make_unique<PriceLevel[]>(domain_.tick_count())},
      asks_{std::make_unique<PriceLevel[]>(domain_.tick_count())},
      bid_occupancy_{domain_.tick_count()}, ask_occupancy_{domain_.tick_count()},
      arena_{max_orders} {}

PriceDomain OrderBook::checked_domain(PriceDomain domain) {
  if (domain.tick_count() > kOrderLevelMask) {
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

SubmitResult OrderBook::validate(Quantity quantity, std::span<Trade> trades) const noexcept {
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
  const SubmitResult validation = validate(quantity, trades);
  if (validation.reject_reason != RejectReason::none) {
    return validation;
  }
  const auto level_index = domain_.index_of(price);
  if (!level_index.has_value()) {
    return rejected(RejectReason::price_out_of_domain, quantity);
  }
  if (arena_.size() == arena_.capacity() && !has_crossing_order(side, *level_index)) {
    return rejected(RejectReason::order_capacity_exhausted, quantity);
  }

  std::uint64_t remaining = quantity.value();
  std::uint32_t trade_count = 0U;
  match(id, side, level_index, remaining, trades, trade_count);
  const Handle handle = remaining == 0U ? kInvalidHandle : rest(id, side, *level_index, remaining);
  return {.reject_reason = RejectReason::none,
          .executed_quantity = Quantity{quantity.value() - remaining},
          .unfilled_quantity = Quantity{remaining},
          .trade_count = trade_count,
          .resting_handle = handle};
}

SubmitResult OrderBook::submit_market(OrderId id, Side side, Quantity quantity,
                                      std::span<Trade> trades) noexcept {
  const SubmitResult validation = validate(quantity, trades);
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
  const auto opposite_best = best_index(side == Side::buy ? Side::sell : Side::buy);
  return opposite_best.has_value() &&
         (side == Side::buy ? *opposite_best <= limit_index : *opposite_best >= limit_index);
}

void OrderBook::match(OrderId taker_id, Side taker_side, std::optional<std::uint32_t> limit_index,
                      std::uint64_t& remaining, std::span<Trade> trades,
                      std::uint32_t& trade_count) noexcept {
  const Side maker_side = taker_side == Side::buy ? Side::sell : Side::buy;
  while (remaining != 0U) {
    const auto maker_level = best_index(maker_side);
    if (!maker_level.has_value()) {
      return;
    }
    if (limit_index.has_value() &&
        (taker_side == Side::buy ? *maker_level > *limit_index : *maker_level < *limit_index)) {
      return;
    }
    match_level(taker_id, taker_side, *maker_level, remaining, trades, trade_count);
  }
}

void OrderBook::match_level(OrderId taker_id, Side taker_side, std::uint32_t level_index,
                            std::uint64_t& remaining, std::span<Trade> trades,
                            std::uint32_t& trade_count) noexcept {
  const Side maker_side = taker_side == Side::buy ? Side::sell : Side::buy;
  PriceLevel& price_level = level(maker_side, level_index);
  const auto execution_price = domain_.price_at(level_index);
  if (!execution_price.has_value()) {
    return;
  }
  while (remaining != 0U && price_level.head_index != kInvalidIndex) {
    Order& maker = arena_.order_at(price_level.head_index);
    const std::uint64_t execution = std::min(remaining, maker.remaining.value());
    trades[trade_count++] = taker_side == Side::buy
                                ? Trade{taker_id, maker.id, *execution_price, Quantity{execution}}
                                : Trade{maker.id, taker_id, *execution_price, Quantity{execution}};
    remaining -= execution;
    price_level.aggregate_quantity -= execution;
    if (execution == maker.remaining.value()) {
      remove_head(maker_side, level_index);
    } else {
      maker.remaining = Quantity{maker.remaining.value() - execution};
    }
  }
}

void OrderBook::remove_head(Side side, std::uint32_t level_index) noexcept {
  PriceLevel& price_level = level(side, level_index);
  const std::uint32_t removed_index = price_level.head_index;
  const std::uint32_t next_index = arena_.order_at(removed_index).next_index;
  price_level.head_index = next_index;
  --price_level.order_count;
  if (next_index == kInvalidIndex) {
    price_level.tail_index = kInvalidIndex;
    static_cast<void>(occupancy(side).clear(level_index));
  } else {
    arena_.order_at(next_index).prev_index = kInvalidIndex;
  }
  arena_.release_index(removed_index);
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
  return side == Side::buy ? bid_occupancy_.last_set() : ask_occupancy_.first_set();
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
  return side == Side::buy ? bids_[index] : asks_[index];
}

const PriceLevel& OrderBook::level(Side side, std::uint32_t index) const noexcept {
  return side == Side::buy ? bids_[index] : asks_[index];
}

HierarchicalBitmap& OrderBook::occupancy(Side side) noexcept {
  return side == Side::buy ? bid_occupancy_ : ask_occupancy_;
}

const HierarchicalBitmap& OrderBook::occupancy(Side side) const noexcept {
  return side == Side::buy ? bid_occupancy_ : ask_occupancy_;
}

} // namespace matching_engine
