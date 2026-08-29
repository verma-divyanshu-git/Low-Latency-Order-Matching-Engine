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
#include <stdexcept>
#include <utility>
#include <vector>

namespace matching_engine::test {

using ModelToken = std::uint64_t;

struct ModelStopActivation {
  OrderId order_id{0U};
  Quantity executed_quantity{0U};
  Quantity unfilled_quantity{0U};
  bool remains_resting{};
};

struct ModelSubmitResult {
  RejectReason reject_reason{RejectReason::none};
  Quantity executed_quantity{0U};
  Quantity unfilled_quantity{0U};
  std::optional<ModelToken> resting_token;
  std::vector<Trade> trades;
  std::vector<ModelStopActivation> stop_activations;
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

struct ModelAuctionResult {
  AuctionResult result;
  std::vector<Trade> trades;
};

class ReferenceOrderBook {
public:
  ReferenceOrderBook(Price minimum, std::uint32_t tick_count, std::size_t max_orders,
                     Quantity max_order_quantity,
                     SelfTradePolicy self_trade_policy = SelfTradePolicy::none,
                     AllocationMode allocation_mode = AllocationMode::fifo,
                     Quantity pro_rata_minimum = Quantity{2U},
                     TradingState trading_state = TradingState::continuous)
      : minimum_{minimum.ticks()},
        maximum_{minimum.ticks() + static_cast<std::int64_t>(tick_count - 1U)},
        max_orders_{max_orders}, max_order_quantity_{max_order_quantity.value()},
        self_trade_policy_{self_trade_policy}, allocation_mode_{allocation_mode},
        pro_rata_minimum_{allocation_mode == AllocationMode::fifo ? 0U
                                                                  : pro_rata_minimum.value()},
        trading_state_{trading_state} {
    if ((allocation_mode != AllocationMode::fifo &&
         allocation_mode != AllocationMode::threshold_pro_rata) ||
        (allocation_mode == AllocationMode::threshold_pro_rata &&
         (pro_rata_minimum.value() == 0U ||
          pro_rata_minimum.value() > max_order_quantity_))) {
      throw std::invalid_argument{"invalid threshold pro-rata configuration"};
    }
    if (trading_state != TradingState::continuous &&
        trading_state != TradingState::opening_auction) {
      throw std::invalid_argument{"invalid trading state"};
    }
  }

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
    if (trading_state_ == TradingState::opening_auction && time_in_force != TimeInForce::gtc) {
      return rejected(RejectReason::invalid_trading_state, quantity);
    }
    if (time_in_force == TimeInForce::fok && available_quantity(side, price) < quantity.value()) {
      return rejected(RejectReason::fok_not_fillable, quantity);
    }
    if (self_trade_policy_ == SelfTradePolicy::cancel_taker &&
        would_self_trade(trader_id, side, price)) {
      return rejected(RejectReason::self_trade_prevented, quantity);
    }
    if (time_in_force == TimeInForce::gtc && live_order_count_ == max_orders_ &&
      (trading_state_ == TradingState::opening_auction || !has_crossing_order(side, price))) {
      return rejected(RejectReason::order_capacity_exhausted, quantity);
    }

    ModelSubmitResult result;
    std::uint64_t remaining = quantity.value();
    if (trading_state_ == TradingState::continuous) {
      match(id, side, price, remaining, result);
    }
    result.executed_quantity = Quantity{quantity.value() - remaining};
    result.unfilled_quantity = Quantity{remaining};
    if (time_in_force == TimeInForce::gtc && remaining != 0U) {
      result.resting_token = rest(id, trader_id, side, price, Quantity{remaining});
    }
    trigger_stops(result);
    return result;
  }

  [[nodiscard]] ModelSubmitResult submit_market(OrderId id, Side side, Quantity quantity,
                                                std::size_t trade_capacity) {
    if (trading_state_ == TradingState::opening_auction) {
      return rejected(RejectReason::invalid_trading_state, quantity);
    }
    const RejectReason invalid = validate(side, quantity, trade_capacity);
    if (invalid != RejectReason::none) {
      return rejected(invalid, quantity);
    }
    ModelSubmitResult result;
    std::uint64_t remaining = quantity.value();
    match(id, side, std::nullopt, remaining, result);
    result.executed_quantity = Quantity{quantity.value() - remaining};
    result.unfilled_quantity = Quantity{remaining};
    trigger_stops(result);
    return result;
  }

  [[nodiscard]] ModelSubmitResult submit_stop(OrderId id, Side side, Price trigger_price,
                                              Quantity quantity, std::size_t trade_capacity) {
    return submit_stop_order(id, side, trigger_price, std::nullopt, quantity, trade_capacity);
  }

  [[nodiscard]] ModelSubmitResult submit_stop_limit(OrderId id, Side side, Price trigger_price,
                                                    Price limit_price, Quantity quantity,
                                                    std::size_t trade_capacity) {
    return submit_stop_order(id, side, trigger_price, limit_price, quantity, trade_capacity);
  }

  [[nodiscard]] ModelSubmitResult submit_post_only(OrderId id, Side side, Price price,
                                                    Quantity quantity,
                                                    std::size_t trade_capacity) {
    if (trading_state_ == TradingState::opening_auction) {
      return rejected(RejectReason::invalid_trading_state, quantity);
    }
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
    if (trading_state_ == TradingState::opening_auction) {
      return rejected(RejectReason::invalid_trading_state, quantity);
    }
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
    trigger_stops(result);
    return result;
  }

  [[nodiscard]] ModelCancelResult cancel(ModelToken token) {
    const auto dormant = std::find_if(stops_.begin(), stops_.end(),
                                      [token](const DormantStop& stop) {
                                        return stop.token == token;
                                      });
    if (dormant != stops_.end()) {
      const ModelCancelResult result{CancelReason::none, dormant->id, dormant->quantity};
      stops_.erase(dormant);
      --live_order_count_;
      return result;
    }
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
    const auto dormant = std::find_if(stops_.begin(), stops_.end(),
                                      [token](const DormantStop& stop) {
                                        return stop.token == token;
                                      });
    if (dormant != stops_.end()) {
      const Quantity previous = dormant->quantity;
      if (new_remaining.value() == 0U) {
        return {AmendReason::zero_quantity, dormant->id, previous, new_remaining, token};
      }
      if (new_remaining.value() > max_order_quantity_) {
        return {AmendReason::quantity_too_large, dormant->id, previous, new_remaining, token};
      }
      if (new_remaining > previous) {
        return {AmendReason::increase_not_allowed, dormant->id, previous, new_remaining, token};
      }
      dormant->quantity = new_remaining;
      return {AmendReason::none, dormant->id, previous, new_remaining, token};
    }
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
    const auto dormant = std::find_if(stops_.begin(), stops_.end(),
                                      [token](const DormantStop& stop) {
                                        return stop.token == token;
                                      });
    if (dormant != stops_.end()) {
      return OrderInfo{dormant->id, dormant->side, dormant->limit_price.value_or(Price{0}),
                       dormant->quantity};
    }
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
    for (const DormantStop& stop : stops_) {
      total += stop.quantity.value();
    }
    return total;
  }

  [[nodiscard]] std::optional<Price> last_execution_price() const noexcept {
    return last_execution_price_;
  }

  [[nodiscard]] TradingState trading_state() const noexcept {
    return trading_state_;
  }

  [[nodiscard]] ModelAuctionResult uncross_opening_auction(std::size_t trade_capacity) {
    ModelAuctionResult output;
    if (trading_state_ != TradingState::opening_auction) {
      output.result.error = AuctionError::not_opening_auction;
      return output;
    }
    if (trade_capacity < max_orders_) {
      output.result.error = AuctionError::insufficient_trade_capacity;
      return output;
    }

    std::optional<std::int64_t> clearing_price;
    std::uint64_t maximum_executable = 0U;
    std::uint64_t minimum_imbalance = std::numeric_limits<std::uint64_t>::max();
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
      const std::int64_t best_bid = bids_.rbegin()->first;
      const std::int64_t best_ask = asks_.begin()->first;
      const std::int64_t reference = best_ask + ((best_bid - best_ask) / 2);
      for (std::int64_t candidate = best_ask; candidate <= best_bid; ++candidate) {
        std::uint64_t eligible_bids = 0U;
        for (auto level = bids_.lower_bound(candidate); level != bids_.end(); ++level) {
          for (const ModelOrder& order : level->second) {
            eligible_bids += order.remaining.value();
          }
        }
        std::uint64_t eligible_asks = 0U;
        for (auto level = asks_.begin(); level != asks_.upper_bound(candidate); ++level) {
          for (const ModelOrder& order : level->second) {
            eligible_asks += order.remaining.value();
          }
        }
        const std::uint64_t executable = std::min(eligible_bids, eligible_asks);
        const std::uint64_t imbalance = eligible_bids > eligible_asks
                                            ? eligible_bids - eligible_asks
                                            : eligible_asks - eligible_bids;
        const auto distance = [reference](std::int64_t price) {
          return price > reference ? price - reference : reference - price;
        };
        if (executable > maximum_executable ||
            (executable == maximum_executable && imbalance < minimum_imbalance) ||
            (executable == maximum_executable && imbalance == minimum_imbalance &&
             (!clearing_price.has_value() || distance(candidate) < distance(*clearing_price))) ||
            (executable == maximum_executable && imbalance == minimum_imbalance &&
             clearing_price.has_value() && distance(candidate) == distance(*clearing_price) &&
             candidate > *clearing_price)) {
          clearing_price = candidate;
          maximum_executable = executable;
          minimum_imbalance = imbalance;
        }
      }
    }

    trading_state_ = TradingState::continuous;
    if (!clearing_price.has_value() || maximum_executable == 0U) {
      return output;
    }
    std::uint64_t remaining = maximum_executable;
    while (remaining != 0U) {
      auto bid_level = std::prev(bids_.end());
      auto ask_level = asks_.begin();
      ModelOrder& bid = bid_level->second.front();
      ModelOrder& ask = ask_level->second.front();
      const std::uint64_t execution =
          std::min({remaining, bid.remaining.value(), ask.remaining.value()});
      output.trades.push_back(
          {bid.id, ask.id, Price{*clearing_price}, Quantity{execution}});
      remaining -= execution;
      bid.remaining = Quantity{bid.remaining.value() - execution};
      ask.remaining = Quantity{ask.remaining.value() - execution};
      bid.displayed_remaining = bid.remaining;
      ask.displayed_remaining = ask.remaining;
      if (bid.remaining.value() == 0U) {
        bid_level->second.pop_front();
        --live_order_count_;
        if (bid_level->second.empty()) {
          bids_.erase(bid_level);
        }
      }
      if (ask.remaining.value() == 0U) {
        ask_level->second.pop_front();
        --live_order_count_;
        if (ask_level->second.empty()) {
          asks_.erase(ask_level);
        }
      }
    }
    last_execution_price_ = Price{*clearing_price};
    output.result = {.clearing_price = Price{*clearing_price},
                     .executed_quantity = Quantity{maximum_executable},
                     .trade_count = static_cast<std::uint32_t>(output.trades.size())};
    return output;
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

  struct DormantStop {
    ModelToken token;
    OrderId id;
    Side side;
    Price trigger_price;
    std::optional<Price> limit_price;
    Quantity quantity;
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

  [[nodiscard]] bool is_triggered(Side side, Price trigger_price) const {
    if (!last_execution_price_.has_value()) {
      return false;
    }
    return side == Side::buy ? *last_execution_price_ >= trigger_price
                             : *last_execution_price_ <= trigger_price;
  }

  [[nodiscard]] ModelSubmitResult submit_stop_order(OrderId id, Side side, Price trigger_price,
                                                    std::optional<Price> limit_price,
                                                    Quantity quantity,
                                                    std::size_t trade_capacity) {
    if (trading_state_ == TradingState::opening_auction) {
      return rejected(RejectReason::invalid_trading_state, quantity);
    }
    if (!is_valid_side(side)) {
      return rejected(RejectReason::invalid_side, quantity);
    }
    if (quantity.value() == 0U) {
      return rejected(RejectReason::zero_quantity, quantity);
    }
    if (quantity.value() > max_order_quantity_) {
      return rejected(RejectReason::quantity_too_large, quantity);
    }
    if (limit_price.has_value() && !contains(*limit_price)) {
      return rejected(RejectReason::price_out_of_domain, quantity);
    }
    if (live_order_count_ == max_orders_) {
      return rejected(RejectReason::order_capacity_exhausted, quantity);
    }
    if (trade_capacity < max_orders_) {
      return rejected(RejectReason::insufficient_trade_capacity, quantity);
    }

    const ModelToken token = next_token_++;
    ++live_order_count_;
    if (is_triggered(side, trigger_price)) {
      ModelSubmitResult result;
      std::uint64_t remaining = quantity.value();
      match(id, side, limit_price, remaining, result);
      result.executed_quantity = Quantity{quantity.value() - remaining};
      result.unfilled_quantity = Quantity{remaining};
      if (limit_price.has_value() && remaining != 0U) {
        rest_existing(token, id, TraderId{0U}, side, *limit_price, Quantity{remaining});
        result.resting_token = token;
      } else {
        --live_order_count_;
      }
      result.stop_activations.push_back(
          {id, Quantity{quantity.value() - remaining}, Quantity{remaining},
           result.resting_token.has_value()});
      trigger_stops(result);
      return result;
    }

    stops_.push_back(DormantStop{token, id, side, trigger_price, limit_price, quantity});
    ModelSubmitResult result;
    result.unfilled_quantity = quantity;
    result.resting_token = token;
    return result;
  }

  void trigger_stops(ModelSubmitResult& result) {
    while (true) {
      const auto triggered = std::find_if(stops_.begin(), stops_.end(),
                                           [this](const DormantStop& stop) {
                                             return is_triggered(stop.side, stop.trigger_price);
                                           });
      if (triggered == stops_.end()) {
        return;
      }
      const DormantStop stop = *triggered;
      stops_.erase(triggered);
      std::uint64_t remaining = stop.quantity.value();
      match(stop.id, stop.side, stop.limit_price, remaining, result);
      bool remains_resting = false;
      if (stop.limit_price.has_value() && remaining != 0U) {
        rest_existing(stop.token, stop.id, TraderId{0U}, stop.side, *stop.limit_price,
                      Quantity{remaining});
        remains_resting = true;
      } else {
        --live_order_count_;
      }
      result.stop_activations.push_back(
          {stop.id, Quantity{stop.quantity.value() - remaining}, Quantity{remaining},
           remains_resting});
    }
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
    if (allocation_mode_ == AllocationMode::threshold_pro_rata) {
      std::uint64_t total_displayed = 0U;
      for (const ModelOrder& maker : makers) {
        total_displayed += maker.displayed_remaining.value();
      }
      const std::uint64_t pro_rata_quantity = remaining;
      std::vector<std::pair<ModelToken, std::uint64_t>> allocations;
      allocations.reserve(makers.size());
      for (const ModelOrder& maker : makers) {
        std::uint64_t execution =
            (maker.displayed_remaining.value() * pro_rata_quantity) / total_displayed;
        execution = std::min(execution, maker.displayed_remaining.value());
        if (execution >= pro_rata_minimum_) {
          allocations.emplace_back(maker.token, execution);
        }
      }
      for (const auto [token, execution] : allocations) {
        const auto maker = std::find_if(makers.begin(), makers.end(), [token](const ModelOrder& order) {
          return order.token == token;
        });
        if (maker != makers.end()) {
          execute_match(taker_id, taker_side, maker_price, makers, maker, execution, remaining,
                        result);
        }
      }
    }
    while (remaining != 0U && !makers.empty()) {
      execute_match(taker_id, taker_side, maker_price, makers, makers.begin(),
                    std::min(remaining, makers.front().displayed_remaining.value()), remaining,
                    result);
    }
  }

  void execute_match(OrderId taker_id, Side taker_side, std::int64_t maker_price, Queue& makers,
                     Queue::iterator maker, std::uint64_t execution, std::uint64_t& remaining,
                     ModelSubmitResult& result) {
      const std::uint64_t maker_before = maker->remaining.value();
      const bool valid_maker =
          maker->token != 0U && maker->side != taker_side && maker_before >= execution;
      result.phantom_fills_valid = result.phantom_fills_valid && valid_maker;
      const Trade trade = taker_side == Side::buy
              ? Trade{taker_id, maker->id, Price{maker_price}, Quantity{execution}}
              : Trade{maker->id, taker_id, Price{maker_price}, Quantity{execution}};
      last_execution_price_ = trade.price;
      const bool valid_ids = taker_side == Side::buy
                                 ? trade.buy_id == taker_id && trade.sell_id == maker->id
                                 : trade.sell_id == taker_id && trade.buy_id == maker->id;
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
        makers.erase(maker);
        --live_order_count_;
      } else {
        maker->remaining = Quantity{maker_before - execution};
        maker->displayed_remaining = Quantity{maker->displayed_remaining.value() - execution};
        if (maker->displayed_remaining.value() == 0U) {
          ModelOrder replenished = *maker;
          replenished.displayed_remaining = Quantity{
              std::min(replenished.display_quantity.value(), replenished.remaining.value())};
          makers.erase(maker);
          makers.push_back(replenished);
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

  void rest_existing(ModelToken token, OrderId id, TraderId trader_id, Side side, Price price,
                     Quantity remaining) {
    Levels& levels = side == Side::buy ? bids_ : asks_;
    levels[price.ticks()].push_back(
        ModelOrder{token, id, trader_id, side, remaining, remaining, remaining});
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
  AllocationMode allocation_mode_;
  std::uint64_t pro_rata_minimum_;
  TradingState trading_state_;
  Levels bids_;
  Levels asks_;
  std::deque<DormantStop> stops_;
  std::optional<Price> last_execution_price_;
  std::size_t live_order_count_{};
  ModelToken next_token_{1U};
};

} // namespace matching_engine::test

#endif
