#ifndef MATCHING_ENGINE_SEQUENCED_ENGINE_HPP
#define MATCHING_ENGINE_SEQUENCED_ENGINE_HPP

#include "matching_engine/command.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace matching_engine {

enum class EngineEventType : std::uint8_t {
  submit_result,
  trade,
  cancel_result,
  amend_result,
  stop_triggered,
};

struct EngineEvent {
  Sequence command_sequence{0U};
  OrderId order_id{0U};
  OrderId secondary_order_id{0U};
  Price price{0};
  Quantity quantity{0U};
  Quantity secondary_quantity{0U};
  Handle handle{};
  std::uint32_t event_index{};
  EngineEventType type{EngineEventType::submit_result};
  std::uint8_t reason{};
  std::uint16_t reserved{};

  [[nodiscard]] static constexpr EngineEvent trade(Sequence sequence, std::uint32_t index,
                                                   const Trade& execution) noexcept {
    return {.command_sequence = sequence,
            .order_id = execution.buy_id,
            .secondary_order_id = execution.sell_id,
            .price = execution.price,
            .quantity = execution.quantity,
            .event_index = index,
            .type = EngineEventType::trade};
  }

  constexpr bool operator==(const EngineEvent&) const noexcept = default;
};

enum class ApplyStatus : std::uint8_t {
  applied,
  invalid_command,
  invalid_sequence,
  sequence_exhausted,
  decreasing_logical_time,
  insufficient_event_capacity,
};

struct ApplyResult {
  ApplyStatus status{ApplyStatus::applied};
  std::size_t event_count{};

  constexpr bool operator==(const ApplyResult&) const noexcept = default;
};

class SequencedEngine {
public:
  SequencedEngine(PriceDomain domain, std::size_t max_orders, Quantity max_order_quantity,
                  Sequence next_sequence = Sequence{1U}, std::uint64_t last_logical_time = 0U,
                  SelfTradePolicy self_trade_policy = SelfTradePolicy::none);

  SequencedEngine(const SequencedEngine&) = delete;
  SequencedEngine& operator=(const SequencedEngine&) = delete;
  SequencedEngine(SequencedEngine&&) = delete;
  SequencedEngine& operator=(SequencedEngine&&) = delete;

  [[nodiscard]] ApplyResult apply(const SequencedCommand& command,
                                  std::span<EngineEvent> events) noexcept;
  [[nodiscard]] std::size_t required_event_capacity(const CommandPayload& payload) const noexcept;
  [[nodiscard]] std::size_t maximum_event_capacity() const noexcept {
    return (trade_capacity_ * 2U) + 1U;
  }
  [[nodiscard]] OrderBook& order_book() noexcept {
    return order_book_;
  }
  [[nodiscard]] const OrderBook& order_book() const noexcept {
    return order_book_;
  }
  [[nodiscard]] Sequence next_sequence() const noexcept {
    return Sequence{next_sequence_};
  }
  [[nodiscard]] std::uint64_t last_logical_time() const noexcept {
    return last_logical_time_;
  }
  [[nodiscard]] bool sequence_exhausted() const noexcept {
    return sequence_exhausted_;
  }

private:
  friend class detail::SnapshotCodec;
  void write_submit_events(const SequencedCommand& command, const SubmitResult& result,
                           std::span<EngineEvent> events) noexcept;

  OrderBook order_book_;
  std::unique_ptr<Trade[]> trade_scratch_;
  std::size_t trade_capacity_;
  std::uint64_t next_sequence_{1U};
  std::uint64_t last_logical_time_{};
  bool sequence_exhausted_{};
};

static_assert(std::is_trivially_copyable_v<EngineEvent>);
static_assert(sizeof(EngineEvent) == 64U);
static_assert(std::has_unique_object_representations_v<EngineEvent>);
static_assert(std::is_trivially_copyable_v<ApplyResult>);

} // namespace matching_engine

#endif
