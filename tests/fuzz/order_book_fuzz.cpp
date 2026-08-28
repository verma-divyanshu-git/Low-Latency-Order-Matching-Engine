#include "matching_engine/order_book.hpp"
#include "reference_order_book.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace matching_engine::test {
namespace {

constexpr std::int64_t kMinimumPrice = 100;
constexpr std::uint32_t kTickCount = 8U;
constexpr std::size_t kCapacity = 8U;
constexpr std::uint64_t kMaxQuantity = 16U;
constexpr std::size_t kBytesPerCommand = 7U;
constexpr std::size_t kMaxCommands = 64U;
constexpr std::size_t kMaxInputBytes = kBytesPerCommand * kMaxCommands;
constexpr Handle kInvalidHandle{std::numeric_limits<std::uint32_t>::max(), 0U};

struct TrackedOrder {
  Handle engine_handle;
  ModelToken model_token;
};

class ByteReader {
public:
  explicit ByteReader(std::span<const std::uint8_t> input) : input_{input} {}

  [[nodiscard]] bool has_command() const noexcept {
    return input_.size() - position_ >= kBytesPerCommand;
  }

  [[nodiscard]] std::uint8_t read() noexcept {
    if (position_ >= input_.size()) {
      std::abort();
    }
    return input_[position_++];
  }

private:
  std::span<const std::uint8_t> input_;
  std::size_t position_{};
};

[[noreturn]] void mismatch() {
  std::abort();
}

void require(bool condition) {
  if (!condition) {
    mismatch();
  }
}

[[nodiscard]] Side decode_side(std::uint8_t byte) {
  return static_cast<Side>(byte % 4U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
}

[[nodiscard]] TimeInForce decode_time_in_force(std::uint8_t byte) {
  return static_cast<TimeInForce>(byte %
                                  5U); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
}

[[nodiscard]] Price decode_price(std::uint8_t byte) {
  const std::uint8_t choice = byte % 12U;
  if (choice < kTickCount) {
    return Price{kMinimumPrice + choice};
  }
  if (choice == 8U) {
    return Price{kMinimumPrice - 1};
  }
  if (choice == 9U) {
    return Price{kMinimumPrice + static_cast<std::int64_t>(kTickCount)};
  }
  if (choice == 10U) {
    return Price{std::numeric_limits<std::int64_t>::min()};
  }
  return Price{std::numeric_limits<std::int64_t>::max()};
}

[[nodiscard]] Quantity decode_quantity(std::uint8_t byte) {
  const std::uint8_t choice = byte % 20U;
  if (choice == 0U) {
    return Quantity{0U};
  }
  if (choice <= kMaxQuantity) {
    return Quantity{choice};
  }
  if (choice == 17U) {
    return Quantity{kMaxQuantity + 1U};
  }
  return Quantity{std::numeric_limits<std::uint64_t>::max()};
}

[[nodiscard]] std::size_t decode_trade_capacity(std::uint8_t byte) {
  switch (byte % 4U) {
  case 0U:
  case 3U:
    return kCapacity;
  case 1U:
    return 0U;
  case 2U:
    return kCapacity - 1U;
  default:
    mismatch();
  }
}

[[nodiscard]] std::pair<Handle, ModelToken>
decode_target(std::uint8_t byte, const std::vector<TrackedOrder>& tracked) {
  const std::size_t candidate =
      static_cast<std::size_t>(byte) % (tracked.size() + static_cast<std::size_t>(2U));
  if (candidate < tracked.size()) {
    return {tracked[candidate].engine_handle, tracked[candidate].model_token};
  }
  return {kInvalidHandle, ModelToken{0U}};
}

void compare_submit(const SubmitResult& engine, const ModelSubmitResult& model,
                    const std::array<Trade, kCapacity>& trades) {
  require(engine.reject_reason == model.reject_reason);
  require(engine.executed_quantity == model.executed_quantity);
  require(engine.unfilled_quantity == model.unfilled_quantity);
  require(static_cast<std::size_t>(engine.trade_count) == model.trades.size());
  require((engine.resting_handle.generation != 0U) == model.resting_token.has_value());
  require(model.phantom_fills_valid);
  for (std::size_t index = 0U; index < model.trades.size(); ++index) {
    require(trades[index] == model.trades[index]);
  }
}

void track_resting(const SubmitResult& engine, const ModelSubmitResult& model,
                   std::vector<TrackedOrder>& tracked) {
  if (engine.resting_handle.generation != 0U && model.resting_token.has_value()) {
    tracked.push_back({engine.resting_handle, *model.resting_token});
  }
}

void compare_state(OrderBook& engine, const ReferenceOrderBook& model,
                   std::vector<TrackedOrder>& tracked) {
  require(engine.best_bid() == model.best_bid());
  require(engine.best_ask() == model.best_ask());
  for (const Side side : {Side::buy, Side::sell}) {
    for (std::int64_t price = kMinimumPrice;
         price < kMinimumPrice + static_cast<std::int64_t>(kTickCount); ++price) {
      require(engine.level_info(side, Price{price}) == model.level_info(side, Price{price}));
    }
  }

  tracked.erase(std::remove_if(tracked.begin(), tracked.end(),
                               [&](const TrackedOrder& order) {
                                 const std::optional<OrderInfo> engine_info =
                                     engine.order_info(order.engine_handle);
                                 const std::optional<OrderInfo> model_info =
                                     model.order_info(order.model_token);
                                 require(engine_info.has_value() == model_info.has_value());
                                 if (engine_info.has_value()) {
                                   require(*engine_info == *model_info);
                                   return false;
                                 }
                                 return true;
                               }),
                tracked.end());
  require(tracked.size() == model.live_order_count());

  const InvariantResult invariants = engine.check_invariants();
  require(invariants.violation == InvariantViolation::none);
  require(static_cast<std::size_t>(invariants.reachable_count) == tracked.size());
}

void execute(ByteReader& reader, OrderId id, OrderBook& engine, ReferenceOrderBook& model,
             std::array<Trade, kCapacity>& trade_storage, std::vector<TrackedOrder>& tracked) {
  const std::uint8_t operation = reader.read() % 6U;
  const Side side = decode_side(reader.read());
  const Price price = decode_price(reader.read());
  const Quantity quantity = decode_quantity(reader.read());
  const TimeInForce time_in_force = decode_time_in_force(reader.read());
  const std::size_t trade_capacity = decode_trade_capacity(reader.read());
  const std::uint8_t target_byte = reader.read();
  const std::span<Trade> trades{trade_storage.data(), trade_capacity};

  switch (operation) {
  case 0U: {
    const SubmitResult engine_result =
        engine.submit_limit(id, side, price, quantity, time_in_force, trades);
    const ModelSubmitResult model_result =
        model.submit_limit(id, side, price, quantity, time_in_force, trade_capacity);
    compare_submit(engine_result, model_result, trade_storage);
    track_resting(engine_result, model_result, tracked);
    break;
  }
  case 1U: {
    const SubmitResult engine_result = engine.submit_market(id, side, quantity, trades);
    const ModelSubmitResult model_result = model.submit_market(id, side, quantity, trade_capacity);
    compare_submit(engine_result, model_result, trade_storage);
    break;
  }
  case 2U: {
    const auto [engine_handle, model_token] = decode_target(target_byte, tracked);
    const CancelResult engine_result = engine.cancel(engine_handle);
    const ModelCancelResult model_result = model.cancel(model_token);
    require(engine_result.reject_reason == model_result.reject_reason);
    require(engine_result.order_id == model_result.order_id);
    require(engine_result.canceled_quantity == model_result.canceled_quantity);
    break;
  }
  case 3U: {
    const auto [engine_handle, model_token] = decode_target(target_byte, tracked);
    const AmendResult engine_result = engine.amend_quantity(engine_handle, quantity);
    const ModelAmendResult model_result = model.amend_quantity(model_token, quantity);
    require(engine_result.reject_reason == model_result.reject_reason);
    require(engine_result.order_id == model_result.order_id);
    require(engine_result.previous_quantity == model_result.previous_quantity);
    require(engine_result.new_quantity == model_result.new_quantity);
    require(engine_result.handle == (model_result.reject_reason == AmendReason::invalid_handle
                                         ? kInvalidHandle
                                         : engine_handle));
    require(
        model_result.token ==
        (model_result.reject_reason == AmendReason::invalid_handle ? ModelToken{0U} : model_token));
    break;
  }
  case 4U: {
    const auto [engine_handle, model_token] = decode_target(target_byte, tracked);
    const SubmitResult engine_result = engine.replace(engine_handle, price, quantity, trades);
    const ModelSubmitResult model_result =
        model.replace(model_token, price, quantity, trade_capacity);
    compare_submit(engine_result, model_result, trade_storage);
    track_resting(engine_result, model_result, tracked);
    break;
  }
  case 5U: {
    const Quantity display_quantity = decode_quantity(target_byte);
    const SubmitResult engine_result =
        engine.submit_iceberg(id, side, price, quantity, display_quantity, trades);
    const ModelSubmitResult model_result =
        model.submit_iceberg(id, TraderId{0U}, side, price, quantity, display_quantity,
                             trade_capacity);
    compare_submit(engine_result, model_result, trade_storage);
    track_resting(engine_result, model_result, tracked);
    break;
  }
  default:
    mismatch();
  }
}

} // namespace
} // namespace matching_engine::test

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace matching_engine;
  using namespace matching_engine::test;

  if (data == nullptr && size != 0U) {
    return 0;
  }
  const std::size_t bounded_size = std::min(size, kMaxInputBytes);
  ByteReader reader{{data, bounded_size}};
  OrderBook engine{PriceDomain{Price{kMinimumPrice}, kTickCount}, kCapacity,
                   Quantity{kMaxQuantity}};
  ReferenceOrderBook model{Price{kMinimumPrice}, kTickCount, kCapacity, Quantity{kMaxQuantity}};
  std::array<Trade, kCapacity> trade_storage{};
  std::vector<TrackedOrder> tracked;
  tracked.reserve(kMaxCommands);

  std::uint64_t next_id = 1U;
  std::size_t command_count = 0U;
  while (reader.has_command() && command_count < kMaxCommands) {
    execute(reader, OrderId{next_id++}, engine, model, trade_storage, tracked);
    compare_state(engine, model, tracked);
    ++command_count;
  }
  return 0;
}
