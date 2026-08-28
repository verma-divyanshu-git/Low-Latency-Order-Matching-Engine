#ifndef MATCHING_ENGINE_TEST_REFERENCE_ORDER_BOOK_HPP
#define MATCHING_ENGINE_TEST_REFERENCE_ORDER_BOOK_HPP

#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace matching_engine::test {

using ModelToken = std::uint64_t;

struct ModelSubmitResult {
  RejectReason reject_reason{RejectReason::none};
  Quantity executed_quantity{0U};
  Quantity unfilled_quantity{0U};
  std::optional<ModelToken> resting_token;
  std::vector<Trade> trades;
  bool phantom_fills_valid{true};
};

struct ModelCancelResult {
  CancelReason reject_reason{CancelReason::none};
  OrderId order_id{0U};
  Quantity canceled_quantity{0U};
};

struct ModelAmendResult {
  AmendReason reject_reason{AmendReason::none};
  OrderId order_id{0U};
  Quantity previous_quantity{0U};
  Quantity new_quantity{0U};
  ModelToken token{};
};

class ReferenceOrderBook {
public:
  ReferenceOrderBook(Price minimum, std::uint32_t tick_count, std::size_t max_orders,
                     Quantity max_order_quantity, SelfTradePolicy self_trade_policy = SelfTradePolicy::none)
      : minimum_{minimum.ticks()},
        maximum_{minimum.ticks() + static_cast<std::int64_t>(tick_count - 1U)},
        max_orders_{max_orders}, max_order_quantity_{max_order_quantity.value()},
        self_trade_policy_{self_trade_policy} {}

  [[nodiscard]] ModelSubmitResult submit_limit(OrderId id, Side side, Price price,
                                               Quantity quantity, TimeInForce time_in_force,
                                               std::size_t trade_capacity) {
    return submit_limit(id, TraderId{0U}, side, price, quantity, time_in_force, trade_capacity);
  }

  [[nodiscard]] ModelSubmitResult submit_limit(OrderId id, TraderId trader_id, Side side,
                                               Price price, Quantity quantity,
                                               TimeInForce time_in_force,
                                               std::size_t trade_capacity) {
    if (!valid_time_in_force(time_in_force)) {
      return rejected(RejectReason::invalid_time_in_force, quantity);
    }
    const RejectReason invalid = validate(side, quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, quantity);
    }
    if (!contains(price)) {
      return rejected(RejectReason::price_out_of_domain, quantity);
    }
    if (time_in_force == TimeInForce::fok && available_quantity(side, price) < quantity.value()) {
      return rejected(RejectReason::fok_not_fillable, quantity);
    }
    if (self_trade_policy_ == SelfTradePolicy::cancel_taker &&
        would_self_trade(trader_id, side, price)) {
      return rejected(RejectReason::self_trade_prevented, quantity);
    }
    if (time_in_force == TimeInForce::gtc && live_order_count_ == max_orders_ &&
        !has_crossing_order(side, price)) {
      return rejected(RejectReason::order_capacity_exhausted, quantity);
    }

    ModelSubmitResult result;
    std::uint64_t remaining = quantity.value();
    match(id, side, price, remaining, result);
    result.executed_quantity = Quantity{quantity.value() - remaining};
    result.unfilled_quantity = Quantity{remaining};
    if (time_in_force == TimeInForce::gtc && remaining != 0U) {
      result.resting_token = rest(id, trader_id, side, price, Quantity{remaining});
    }
    return result;
  }

  [[nodiscard]] ModelSubmitResult submit_market(OrderId id, Side side, Quantity quantity,
                                                std::size_t trade_capacity) {
    const RejectReason invalid = validate(side, quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, quantity);
    }
    ModelSubmitResult result;
    std::uint64_t remaining = quantity.value();
    match(id, side, std::nullopt, remaining, result);
    result.executed_quantity = Quantity{quantity.value() - remaining};
    result.unfilled_quantity = Quantity{remaining};
    return result;
  }

  [[nodiscard]] ModelSubmitResult submit_post_only(OrderId id, Side side, Price price,
                                                    Quantity quantity,
                                                    std::size_t trade_capacity) {
    const RejectReason invalid = validate(side, quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, quantity);
    }
    if (!contains(price)) {
      return rejected(RejectReason::price_out_of_domain, quantity);
    }
    if (has_crossing_order(side, price)) {
      return rejected(RejectReason::post_only_would_cross, quantity);
    }
    return submit_limit(id, side, price, quantity, TimeInForce::gtc, trade_capacity);
  }

  [[nodiscard]] ModelSubmitResult submit_iceberg(OrderId id, TraderId trader_id, Side side,
                                                  Price price, Quantity quantity,
                                                  Quantity display_quantity,
                                                  std::size_t trade_capacity) {
    if (display_quantity.value() == 0U || display_quantity > quantity) {
      return rejected(RejectReason::invalid_display_quantity, quantity);
    }
    const RejectReason invalid = validate(side, quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, quantity);
    }
    if (!contains(price)) {
      return rejected(RejectReason::price_out_of_domain, quantity);
    }
    if (live_order_count_ == max_orders_ && !has_crossing_order(side, price)) {
      return rejected(RejectReason::order_capacity_exhausted, quantity);
    }
    if (self_trade_policy_ == SelfTradePolicy::cancel_taker &&
        would_self_trade(trader_id, side, price)) {
      return rejected(RejectReason::self_trade_prevented, quantity);
    }
    ModelSubmitResult result;
    std::uint64_t remaining = quantity.value();
    match(id, side, price, remaining, result);
    result.executed_quantity = Quantity{quantity.value() - remaining};
    result.unfilled_quantity = Quantity{remaining};
    if (remaining != 0U) {
      result.resting_token = rest(id, trader_id, side, price, Quantity{remaining},
                                  Quantity{std::min(display_quantity.value(), remaining)});
    }
    return result;
  }

  [[nodiscard]] ModelCancelResult cancel(ModelToken token) {
    const auto location = locate(token);
    if (!location.has_value()) {
      return {CancelReason::invalid_handle, OrderId{0U}, Quantity{0U}};
    }
    const OrderId id = location->order->id;
    const Quantity remaining = location->order->remaining;
    location->level->second.erase(location->order);
    if (location->level->second.empty()) {
      location->levels->erase(location->level);
    }
    --live_order_count_;
    return {CancelReason::none, id, remaining};
  }

  [[nodiscard]] ModelAmendResult amend_quantity(ModelToken token, Quantity new_remaining) {
    const auto location = locate(token);
    if (!location.has_value()) {
      return {AmendReason::invalid_handle, OrderId{0U}, Quantity{0U}, new_remaining,
              ModelToken{0U}};
    }
    const Quantity previous = location->order->remaining;
    if (new_remaining.value() == 0U) {
      return {AmendReason::zero_quantity, location->order->id, previous, new_remaining, token};
    }
    if (new_remaining.value() > max_order_quantity_) {
      return {AmendReason::quantity_too_large, location->order->id, previous, new_remaining, token};
    }
    if (new_remaining > previous) {
      return {AmendReason::increase_not_allowed, location->order->id, previous, new_remaining,
              token};
    }
    location->order->remaining = new_remaining;
    location->order->displayed_remaining =
      std::min(location->order->displayed_remaining, new_remaining);
    return {AmendReason::none, location->order->id, previous, new_remaining, token};
  }

  [[nodiscard]] ModelSubmitResult replace(ModelToken token, Price new_price, Quantity new_quantity,
                                          std::size_t trade_capacity) {
    const auto location = locate(token);
    if (!location.has_value()) {
      return rejected(RejectReason::invalid_handle, new_quantity);
    }
    const Side side = location->order->side;
    const OrderId id = location->order->id;
    const RejectReason invalid = validate(side, new_quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, new_quantity);
    }
    if (!contains(new_price)) {
      return rejected(RejectReason::price_out_of_domain, new_quantity);
    }
    static_cast<void>(cancel(token));
    return submit_limit(id, side, new_price, new_quantity, TimeInForce::gtc, trade_capacity);
  }

  [[nodiscard]] std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional{Price{bids_.rbegin()->first}};
  }

  [[nodiscard]] std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional{Price{asks_.begin()->first}};
  }

  [[nodiscard]] std::optional<LevelInfo> level_info(Side side, Price price) const {
    if (!is_valid_side(side) || !contains(price)) {
      return std::nullopt;
    }
    const Levels& levels = side == Side::buy ? bids_ : asks_;
    const auto found = levels.find(price.ticks());
    if (found == levels.end()) {
      return LevelInfo{};
    }
    std::uint64_t aggregate = 0U;
    for (const ModelOrder& order : found->second) {
      aggregate += order.remaining.value();
    }
    return LevelInfo{Quantity{aggregate}, static_cast<std::uint32_t>(found->second.size())};
  }

  [[nodiscard]] std::optional<OrderInfo> order_info(ModelToken token) const {
    for (const auto& [price, orders] : bids_) {
      for (const ModelOrder& order : orders) {
        if (order.token == token) {
          return OrderInfo{order.id, order.side, Price{price}, order.remaining};
        }
      }
    }
    for (const auto& [price, orders] : asks_) {
      for (const ModelOrder& order : orders) {
        if (order.token == token) {
          return OrderInfo{order.id, order.side, Price{price}, order.remaining};
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::size_t live_order_count() const noexcept {
    return live_order_count_;
  }

  [[nodiscard]] std::uint64_t resting_quantity() const {
    std::uint64_t total = 0U;
    for (const Levels* levels : {&bids_, &asks_}) {
      for (const auto& [price, orders] : *levels) {
        static_cast<void>(price);
        for (const ModelOrder& order : orders) {
          total += order.remaining.value();
        }
      }
    }
    return total;
  }

private:
  struct ModelOrder {
    ModelToken token;
    OrderId id;
    TraderId trader_id;
    Side side;
    Quantity remaining;
    Quantity display_quantity;
    Quantity displayed_remaining;
  };

  using Queue = std::deque<ModelOrder>;
  using Levels = std::map<std::int64_t, Queue>;

  struct Location {
    Levels* levels;
    Levels::iterator level;
    Queue::iterator order;
  };

  [[nodiscard]] static bool valid_time_in_force(TimeInForce time_in_force) {
    return time_in_force == TimeInForce::gtc || time_in_force == TimeInForce::ioc ||
           time_in_force == TimeInForce::fok;
  }

  [[nodiscard]] bool contains(Price price) const {
    return price.ticks() >= minimum_ && price.ticks() <= maximum_;
  }

  [[nodiscard]] RejectReason validate(Side side, Quantity quantity,
                                      std::size_t trade_capacity) const {
    if (!is_valid_side(side)) {
      return RejectReason::invalid_side;
    }
    if (quantity.value() == 0U) {
      return RejectReason::zero_quantity;
    }
    if (quantity.value() > max_order_quantity_) {
      return RejectReason::quantity_too_large;
    }
    if (trade_capacity < max_orders_) {
      return RejectReason::insufficient_trade_capacity;
    }
    return RejectReason::none;
  }

  [[nodiscard]] static ModelSubmitResult rejected(RejectReason reason, Quantity quantity) {
    ModelSubmitResult result;
    result.reject_reason = reason;
    result.unfilled_quantity = quantity;
    return result;
  }

  [[nodiscard]] bool has_crossing_order(Side side, Price price) const {
    if (side == Side::buy) {
      return !asks_.empty() && asks_.begin()->first <= price.ticks();
    }
    return !bids_.empty() && bids_.rbegin()->first >= price.ticks();
  }

  [[nodiscard]] bool would_self_trade(TraderId trader_id, Side side, Price limit) const {
    const Levels& makers = side == Side::buy ? asks_ : bids_;
    if (side == Side::buy) {
      for (const auto& [price, orders] : makers) {
        if (price > limit.ticks()) {
          break;
        }
        for (const ModelOrder& order : orders) {
          if (order.trader_id == trader_id) {
            return true;
          }
        }
      }
    } else {
      for (auto level = makers.rbegin(); level != makers.rend(); ++level) {
        if (level->first < limit.ticks()) {
          break;
        }
        for (const ModelOrder& order : level->second) {
          if (order.trader_id == trader_id) {
            return true;
          }
        }
      }
    }
    return false;
  }

  [[nodiscard]] std::uint64_t available_quantity(Side side, Price limit) const {
    std::uint64_t available = 0U;
    const Levels& makers = side == Side::buy ? asks_ : bids_;
    if (side == Side::buy) {
      for (const auto& [price, orders] : makers) {
        if (price > limit.ticks()) {
          break;
        }
        for (const ModelOrder& order : orders) {
          available += order.remaining.value();
        }
      }
    } else {
      for (auto level = makers.rbegin(); level != makers.rend(); ++level) {
        if (level->first < limit.ticks()) {
          break;
        }
        for (const ModelOrder& order : level->second) {
          available += order.remaining.value();
        }
      }
    }
    return available;
  }

  void match(OrderId taker_id, Side taker_side, std::optional<Price> limit,
             std::uint64_t& remaining, ModelSubmitResult& result) {
    while (remaining != 0U) {
      if (taker_side == Side::buy) {
        if (asks_.empty() || (limit.has_value() && asks_.begin()->first > limit->ticks())) {
          return;
        }
        auto level = asks_.begin();
        match_level(taker_id, taker_side, level->first, level->second, remaining, result);
        if (level->second.empty()) {
          asks_.erase(level);
        }
      } else {
        if (bids_.empty() || (limit.has_value() && bids_.rbegin()->first < limit->ticks())) {
          return;
        }
        auto level = std::prev(bids_.end());
        match_level(taker_id, taker_side, level->first, level->second, remaining, result);
        if (level->second.empty()) {
          bids_.erase(level);
        }
      }
    }
  }

  void match_level(OrderId taker_id, Side taker_side, std::int64_t maker_price, Queue& makers,
                   std::uint64_t& remaining, ModelSubmitResult& result) {
    while (remaining != 0U && !makers.empty()) {
      ModelOrder& maker = makers.front();
      const std::uint64_t maker_before = maker.remaining.value();
      const std::uint64_t execution = std::min(remaining, maker.displayed_remaining.value());
      const bool valid_maker =
          maker.token != 0U && maker.side != taker_side && maker_before >= execution;
      result.phantom_fills_valid = result.phantom_fills_valid && valid_maker;
      const Trade trade = taker_side == Side::buy
              ? Trade{taker_id, maker.id, Price{maker_price}, Quantity{execution}}
              : Trade{maker.id, taker_id, Price{maker_price}, Quantity{execution}};
      const bool valid_ids = taker_side == Side::buy
                                 ? trade.buy_id == taker_id && trade.sell_id == maker.id
                                 : trade.sell_id == taker_id && trade.buy_id == maker.id;
      result.phantom_fills_valid = result.phantom_fills_valid && valid_ids;
      const auto existing = std::find_if(result.trades.begin(), result.trades.end(),
                                         [&](const Trade& candidate) {
                                           return candidate.buy_id == trade.buy_id &&
                                                  candidate.sell_id == trade.sell_id &&
                                                  candidate.price == trade.price;
                                         });
      if (existing == result.trades.end()) {
        result.trades.push_back(trade);
      } else {
        existing->quantity = Quantity{existing->quantity.value() + execution};
      }
      remaining -= execution;
      if (execution == maker_before) {
        makers.pop_front();
        --live_order_count_;
      } else {
        maker.remaining = Quantity{maker_before - execution};
        maker.displayed_remaining = Quantity{maker.displayed_remaining.value() - execution};
        if (maker.displayed_remaining.value() == 0U) {
            ModelOrder replenished = maker;
            replenished.displayed_remaining = Quantity{
              std::min(replenished.display_quantity.value(), replenished.remaining.value())};
          makers.pop_front();
            makers.push_back(replenished);
        }
      }
    }
  }

  [[nodiscard]] ModelToken rest(OrderId id, TraderId trader_id, Side side, Price price,
                                 Quantity remaining) {
    return rest(id, trader_id, side, price, remaining, remaining);
  }

  [[nodiscard]] ModelToken rest(OrderId id, TraderId trader_id, Side side, Price price,
                                 Quantity remaining, Quantity display_quantity) {
    const ModelToken token = next_token_++;
    Levels& levels = side == Side::buy ? bids_ : asks_;
    levels[price.ticks()].push_back(
      ModelOrder{token, id, trader_id, side, remaining, display_quantity, display_quantity});
    ++live_order_count_;
    return token;
  }

  [[nodiscard]] std::optional<Location> locate(ModelToken token) {
    for (Levels* levels : {&bids_, &asks_}) {
      for (auto level = levels->begin(); level != levels->end(); ++level) {
        const auto order =
            std::find_if(level->second.begin(), level->second.end(),
                         [token](const ModelOrder& candidate) { return candidate.token == token; });
        if (order != level->second.end()) {
          return Location{levels, level, order};
        }
      }
    }
    return std::nullopt;
  }

  std::int64_t minimum_;
  std::int64_t maximum_;
  std::size_t max_orders_;
  std::uint64_t max_order_quantity_;
  SelfTradePolicy self_trade_policy_;
  Levels bids_;
  Levels asks_;
  std::size_t live_order_count_{};
  ModelToken next_token_{1U};
};

} // namespace matching_engine::test

#endif
