#ifndef MATCHING_ENGINE_TEST_DIFFERENTIAL_SIMULATOR_HPP
#define MATCHING_ENGINE_TEST_DIFFERENTIAL_SIMULATOR_HPP

#include "reference_order_book.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace matching_engine::test {

[[nodiscard]] inline std::optional<std::uint64_t> parse_replay_seed(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t seed = 0U;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed, 10);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return seed;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add(std::uint64_t left,
                                                                 std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::nullopt;
  }
  return left + right;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_double(std::uint64_t value) {
  return value > std::numeric_limits<std::uint64_t>::max() / 2U
             ? std::nullopt
             : std::optional<std::uint64_t>{value * 2U};
}

enum class CommandKind : std::uint8_t {
  limit,
  market,
  cancel,
  amend,
  replace,
};

struct Command {
  CommandKind kind{CommandKind::limit};
  OrderId id{0U};
  Side side{Side::buy};
  Price price{0};
  Quantity quantity{0U};
  TimeInForce time_in_force{TimeInForce::gtc};
  std::size_t target{};
  bool full_output{true};
};

inline std::ostream& operator<<(std::ostream& output, const Command& command) {
  return output << "kind=" << static_cast<unsigned>(command.kind) << " id=" << command.id.value()
                << " side=" << static_cast<unsigned>(command.side)
                << " price=" << command.price.ticks() << " qty=" << command.quantity.value()
                << " tif=" << static_cast<unsigned>(command.time_in_force)
                << " target=" << command.target << " full_output=" << command.full_output;
}

struct Scenario {
  const char* name;
  std::array<std::uint32_t, 5> weights;
  std::array<std::uint32_t, 3> time_in_force_weights;
  bool shock_prices;

  [[nodiscard]] static constexpr Scenario normal() {
    return {"normal", {42U, 13U, 16U, 15U, 14U}, {55U, 25U, 20U}, false};
  }

  [[nodiscard]] static constexpr Scenario high_cancel() {
    return {"synthetic-high-cancel", {25U, 8U, 47U, 10U, 10U}, {60U, 25U, 15U}, false};
  }

  [[nodiscard]] static constexpr Scenario volatility_shock() {
    return {"synthetic-volatility-shock", {53U, 17U, 8U, 8U, 14U}, {45U, 30U, 25U}, true};
  }
};

// SplitMix64 is used directly so command streams do not depend on
// implementation-defined standard-library random-distribution algorithms.
class DeterministicRandom {
public:
  explicit DeterministicRandom(std::uint64_t seed) : state_{seed} {}

  [[nodiscard]] std::uint64_t next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) {
    return bound == 0U ? 0U : next() % bound;
  }

private:
  std::uint64_t state_;
};

class DifferentialSimulator {
public:
  explicit DifferentialSimulator(Scenario scenario,
                                 AllocationMode allocation_mode = AllocationMode::fifo,
                                 Quantity pro_rata_minimum = Quantity{2U})
      : scenario_{scenario}, allocation_mode_{allocation_mode},
        pro_rata_minimum_{pro_rata_minimum} {}

  void run(std::uint64_t seed, std::uint32_t operation_count) {
    DeterministicRandom random{seed};
    OrderBook engine{PriceDomain{Price{kMinimumPrice}, kTickCount}, kCapacity,
                     Quantity{kMaxQuantity}, SelfTradePolicy::none, allocation_mode_,
                     pro_rata_minimum_};
    ReferenceOrderBook model{Price{kMinimumPrice}, kTickCount, kCapacity, Quantity{kMaxQuantity},
                             SelfTradePolicy::none, allocation_mode_, pro_rata_minimum_};
    std::vector<Trade> trade_buffer(kCapacity);
    std::vector<TrackedOrder> tracked;
    Ledger ledger;
    std::uint64_t next_id = 1U;

    for (std::uint32_t step = 0U; step < operation_count; ++step) {
      const Command command = generate(random, step, next_id++, tracked.size());
      SCOPED_TRACE(::testing::Message()
                   << "replay: ORDER_BOOK_DIFF_SEED=" << seed
                   << " ./build/debug/matching_engine_core_tests --gtest_filter=" << replay_filter()
                   << " scenario=" << scenario_.name << " seed=" << seed << " step=" << step
                   << " command={" << command << '}');
      execute(command, engine, model, trade_buffer, tracked, ledger, seed, step);
      compare_state(engine, model, tracked);
      ledger.expect_conserved(model.resting_quantity(), seed, step);
    }
  }

private:
  static constexpr std::int64_t kMinimumPrice = 0;
  static constexpr std::uint32_t kTickCount = 101U;
  static constexpr std::size_t kCapacity = 32U;
  static constexpr std::uint64_t kMaxQuantity = 50U;
  static constexpr Handle kInvalidHandle{std::numeric_limits<std::uint32_t>::max(), 0U};

  [[nodiscard]] const char* replay_filter() const {
    if (scenario_.shock_prices) {
      return "OrderBookDifferentialSyntheticStressTest.VolatilityShockWorkload";
    }
    if (scenario_.weights[2] == Scenario::high_cancel().weights[2]) {
      return "OrderBookDifferentialSyntheticStressTest.HighCancelWorkload";
    }
    return "OrderBookDifferentialTest.NormalSeedRegressionCorpusCoversTenThousandOperations";
  }

  struct TrackedOrder {
    Handle engine_handle;
    ModelToken model_token;
  };

  struct Ledger {
    std::uint64_t accepted{};
    std::uint64_t traded{};
    std::uint64_t canceled{};

    void add_accepted(std::uint64_t quantity, std::uint64_t seed, std::uint32_t step) {
      accumulate(accepted, quantity, "accepted submitted", seed, step);
    }

    void add_traded(std::uint64_t quantity, std::uint64_t seed, std::uint32_t step) {
      accumulate(traded, quantity, "traded", seed, step);
    }

    void add_canceled(std::uint64_t quantity, std::uint64_t seed, std::uint32_t step) {
      accumulate(canceled, quantity, "canceled/unfilled", seed, step);
    }

    void expect_conserved(std::uint64_t resting, std::uint64_t seed, std::uint32_t step) const {
      const auto doubled = checked_double(traded);
      if (!doubled.has_value()) {
        ADD_FAILURE() << "conservation arithmetic overflow: accepted_submitted=" << accepted
                      << " traded_quantity=" << traded << " doubled_traded_contribution=overflow"
                      << " canceled_unfilled=" << canceled << " resting=" << resting
                      << " expected_rhs=overflow seed=" << seed << " step=" << step;
        return;
      }
      const auto canceled_total = checked_add(*doubled, canceled);
      const auto expected =
          canceled_total.has_value() ? checked_add(*canceled_total, resting) : std::nullopt;
      if (!expected.has_value()) {
        ADD_FAILURE() << "conservation arithmetic overflow: accepted_submitted=" << accepted
                      << " traded_quantity=" << traded
                      << " doubled_traded_contribution=" << *doubled
                      << " canceled_unfilled=" << canceled << " resting=" << resting
                      << " expected_rhs=overflow seed=" << seed << " step=" << step;
        return;
      }
      EXPECT_EQ(accepted, *expected)
          << "accepted_submitted=" << accepted << " traded_quantity=" << traded
          << " doubled_traded_contribution=" << *doubled << " canceled_unfilled=" << canceled
          << " resting=" << resting << " expected_rhs=" << *expected << " seed=" << seed
          << " step=" << step;
    }

  private:
    static void accumulate(std::uint64_t& total, std::uint64_t quantity, const char* term,
                           std::uint64_t seed, std::uint32_t step) {
      const auto sum = checked_add(total, quantity);
      if (!sum.has_value()) {
        ADD_FAILURE() << "ledger overflow: term=" << term << " current=" << total
                      << " increment=" << quantity << " seed=" << seed << " step=" << step;
        return;
      }
      total = *sum;
    }
  };

  [[nodiscard]] Command generate(DeterministicRandom& random, std::uint32_t step,
                                 std::uint64_t next_id, std::size_t live_count) const {
    Command command;
    command.kind = choose_kind(random);
    command.id = OrderId{next_id};
    command.side = random.bounded(2U) == 0U ? Side::buy : Side::sell;
    command.price = Price{generated_price(random, step)};
    command.quantity = Quantity{1U + random.bounded(kMaxQuantity)};
    command.time_in_force = choose_time_in_force(random);
    command.full_output = random.bounded(20U) != 0U;
    command.target =
        live_count != 0U && random.bounded(10U) != 0U
            ? static_cast<std::size_t>(random.bounded(static_cast<std::uint64_t>(live_count)))
            : live_count + 1U;

    const std::uint64_t rejection = random.bounded(40U);
    if (rejection == 0U) {
      command.quantity = Quantity{0U};
    } else if (rejection == 1U) {
      command.quantity = Quantity{kMaxQuantity + 1U};
    } else if (rejection == 2U) {
      command.side = static_cast<Side>(2U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    } else if (rejection == 3U) {
      command.price = Price{kMinimumPrice - 1};
    }
    return command;
  }

  [[nodiscard]] CommandKind choose_kind(DeterministicRandom& random) const {
    std::uint64_t choice =
        random.bounded(static_cast<std::uint64_t>(scenario_.weights[0]) + scenario_.weights[1] +
                       scenario_.weights[2] + scenario_.weights[3] + scenario_.weights[4]);
    for (std::size_t index = 0U; index < scenario_.weights.size(); ++index) {
      if (choice < scenario_.weights[index]) {
        return static_cast<CommandKind>(index);
      }
      choice -= scenario_.weights[index];
    }
    return CommandKind::limit;
  }

  [[nodiscard]] TimeInForce choose_time_in_force(DeterministicRandom& random) const {
    if (random.bounded(50U) == 0U) {
      return static_cast<TimeInForce>(3U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    }
    std::uint64_t choice =
        random.bounded(static_cast<std::uint64_t>(scenario_.time_in_force_weights[0]) +
                       scenario_.time_in_force_weights[1] + scenario_.time_in_force_weights[2]);
    for (std::size_t index = 0U; index < scenario_.time_in_force_weights.size(); ++index) {
      if (choice < scenario_.time_in_force_weights[index]) {
        return static_cast<TimeInForce>(index);
      }
      choice -= scenario_.time_in_force_weights[index];
    }
    return TimeInForce::gtc;
  }

  [[nodiscard]] std::int64_t generated_price(DeterministicRandom& random,
                                             std::uint32_t step) const {
    if (!scenario_.shock_prices) {
      return static_cast<std::int64_t>(random.bounded(kTickCount));
    }
    const std::int64_t center = ((step / 100U) % 2U) == 0U ? 20 : 80;
    return center - 5 + static_cast<std::int64_t>(random.bounded(11U));
  }

  static void compare_submit(const SubmitResult& engine, const ModelSubmitResult& model,
                             const std::vector<Trade>& trade_buffer) {
    EXPECT_EQ(engine.reject_reason, model.reject_reason);
    EXPECT_EQ(engine.executed_quantity, model.executed_quantity);
    EXPECT_EQ(engine.unfilled_quantity, model.unfilled_quantity);
    EXPECT_EQ(engine.trade_count, model.trades.size());
    EXPECT_EQ(engine.resting_handle.generation != 0U, model.resting_token.has_value());
    EXPECT_TRUE(model.phantom_fills_valid);
    const std::size_t count =
        std::min(static_cast<std::size_t>(engine.trade_count), model.trades.size());
    for (std::size_t index = 0U; index < count; ++index) {
      EXPECT_EQ(trade_buffer[index], model.trades[index]);
    }
  }

  static void record_submit(const Command& command, const ModelSubmitResult& result, Ledger& ledger,
                            std::uint64_t seed, std::uint32_t step) {
    if (result.reject_reason != RejectReason::none) {
      return;
    }
    ledger.add_accepted(command.quantity.value(), seed, step);
    for (const Trade& trade : result.trades) {
      ledger.add_traded(trade.quantity.value(), seed, step);
    }
    if (!result.resting_token.has_value()) {
      ledger.add_canceled(result.unfilled_quantity.value(), seed, step);
    }
  }

  static void track_resting(const SubmitResult& engine, const ModelSubmitResult& model,
                            std::vector<TrackedOrder>& tracked) {
    if (engine.resting_handle.generation != 0U && model.resting_token.has_value()) {
      tracked.push_back({engine.resting_handle, *model.resting_token});
    }
  }

  static std::span<Trade> output_span(const Command& command, std::vector<Trade>& buffer) {
    const std::size_t size = command.full_output ? buffer.size() : buffer.size() - 1U;
    return {buffer.data(), size};
  }

  static void execute(const Command& command, OrderBook& engine, ReferenceOrderBook& model,
                      std::vector<Trade>& trade_buffer, std::vector<TrackedOrder>& tracked,
                      Ledger& ledger, std::uint64_t seed, std::uint32_t step) {
    const std::size_t trade_capacity =
        command.full_output ? trade_buffer.size() : trade_buffer.size() - 1U;
    const std::span<Trade> trades = output_span(command, trade_buffer);
    switch (command.kind) {
    case CommandKind::limit: {
      const SubmitResult engine_result = engine.submit_limit(
          command.id, command.side, command.price, command.quantity, command.time_in_force, trades);
      const ModelSubmitResult model_result =
          model.submit_limit(command.id, command.side, command.price, command.quantity,
                             command.time_in_force, trade_capacity);
      compare_submit(engine_result, model_result, trade_buffer);
      record_submit(command, model_result, ledger, seed, step);
      track_resting(engine_result, model_result, tracked);
      return;
    }
    case CommandKind::market: {
      const SubmitResult engine_result =
          engine.submit_market(command.id, command.side, command.quantity, trades);
      const ModelSubmitResult model_result =
          model.submit_market(command.id, command.side, command.quantity, trade_capacity);
      compare_submit(engine_result, model_result, trade_buffer);
      record_submit(command, model_result, ledger, seed, step);
      return;
    }
    case CommandKind::cancel: {
      const auto [engine_handle, model_token] = target(command, tracked);
      const CancelResult engine_result = engine.cancel(engine_handle);
      const ModelCancelResult model_result = model.cancel(model_token);
      EXPECT_EQ(engine_result.reject_reason, model_result.reject_reason);
      EXPECT_EQ(engine_result.order_id, model_result.order_id);
      EXPECT_EQ(engine_result.canceled_quantity, model_result.canceled_quantity);
      if (model_result.reject_reason == CancelReason::none) {
        ledger.add_canceled(model_result.canceled_quantity.value(), seed, step);
      }
      return;
    }
    case CommandKind::amend: {
      const auto [engine_handle, model_token] = target(command, tracked);
      const AmendResult engine_result = engine.amend_quantity(engine_handle, command.quantity);
      const ModelAmendResult model_result = model.amend_quantity(model_token, command.quantity);
      EXPECT_EQ(engine_result.reject_reason, model_result.reject_reason);
      EXPECT_EQ(engine_result.order_id, model_result.order_id);
      EXPECT_EQ(engine_result.previous_quantity, model_result.previous_quantity);
      EXPECT_EQ(engine_result.new_quantity, model_result.new_quantity);
      EXPECT_EQ(engine_result.handle, engine_handle);
      EXPECT_EQ(model_result.token, model_token);
      if (model_result.reject_reason == AmendReason::invalid_handle) {
        EXPECT_EQ(engine_result.handle, kInvalidHandle);
        EXPECT_EQ(model_result.token, ModelToken{0U});
      }
      if (model_result.reject_reason == AmendReason::none) {
        ledger.add_canceled(
            model_result.previous_quantity.value() - model_result.new_quantity.value(), seed, step);
      }
      return;
    }
    case CommandKind::replace: {
      const auto [engine_handle, model_token] = target(command, tracked);
      const auto old_info = model.order_info(model_token);
      const SubmitResult engine_result =
          engine.replace(engine_handle, command.price, command.quantity, trades);
      const ModelSubmitResult model_result =
          model.replace(model_token, command.price, command.quantity, trade_capacity);
      compare_submit(engine_result, model_result, trade_buffer);
      if (model_result.reject_reason == RejectReason::none) {
        ASSERT_TRUE(old_info.has_value());
        ledger.add_canceled(old_info->remaining.value(), seed, step);
        record_submit(command, model_result, ledger, seed, step);
      }
      track_resting(engine_result, model_result, tracked);
      return;
    }
    }
  }

  [[nodiscard]] static std::pair<Handle, ModelToken>
  target(const Command& command, const std::vector<TrackedOrder>& tracked) {
    return command.target < tracked.size() ? std::pair{tracked[command.target].engine_handle,
                                                       tracked[command.target].model_token}
                                           : std::pair{kInvalidHandle, ModelToken{0U}};
  }

  static void compare_state(OrderBook& engine, const ReferenceOrderBook& model,
                            std::vector<TrackedOrder>& tracked) {
    EXPECT_EQ(engine.best_bid(), model.best_bid());
    EXPECT_EQ(engine.best_ask(), model.best_ask());
    for (const Side side : {Side::buy, Side::sell}) {
      for (std::int64_t price = kMinimumPrice;
           price < kMinimumPrice + static_cast<std::int64_t>(kTickCount); ++price) {
        EXPECT_EQ(engine.level_info(side, Price{price}), model.level_info(side, Price{price}));
      }
    }

    tracked.erase(std::remove_if(tracked.begin(), tracked.end(),
                                 [&](const TrackedOrder& order) {
                                   const auto engine_info = engine.order_info(order.engine_handle);
                                   const auto model_info = model.order_info(order.model_token);
                                   EXPECT_EQ(engine_info.has_value(), model_info.has_value());
                                   if (engine_info.has_value() && model_info.has_value()) {
                                     EXPECT_EQ(*engine_info, *model_info);
                                     return false;
                                   }
                                   return true;
                                 }),
                  tracked.end());
    EXPECT_EQ(tracked.size(), model.live_order_count());
    const InvariantResult invariants = engine.check_invariants();
    EXPECT_EQ(invariants.violation, InvariantViolation::none);
    EXPECT_EQ(invariants.reachable_count, tracked.size());
  }

  Scenario scenario_;
  AllocationMode allocation_mode_;
  Quantity pro_rata_minimum_;
};

} // namespace matching_engine::test

#endif
