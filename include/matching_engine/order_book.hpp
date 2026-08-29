#ifndef MATCHING_ENGINE_ORDER_BOOK_HPP
#define MATCHING_ENGINE_ORDER_BOOK_HPP

#include "matching_engine/hierarchical_bitmap.hpp"
#include "matching_engine/order_arena.hpp"
#include "matching_engine/price_domain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace matching_engine {

inline constexpr std::uint32_t kMaximumPriceLevels = 1'000'000U;

struct PriceLevel {
  std::uint32_t head_index{kInvalidIndex};
  std::uint32_t tail_index{kInvalidIndex};
  std::uint64_t aggregate_quantity{};
  std::uint32_t order_count{};
};

struct LevelInfo {
  Quantity aggregate_quantity{0U};
  std::uint32_t order_count{};

  constexpr bool operator==(const LevelInfo&) const noexcept = default;
};

struct OrderInfo {
  OrderId id{0U};
  Side side{Side::buy};
  Price price{0};
  Quantity remaining{0U};

  constexpr bool operator==(const OrderInfo&) const noexcept = default;
};

struct Trade {
  OrderId buy_id{0U};
  OrderId sell_id{0U};
  Price price{0};
  Quantity quantity{0U};

  constexpr bool operator==(const Trade&) const noexcept = default;
};

enum class TimeInForce : std::uint8_t {
  gtc,
  ioc,
  fok,
};

enum class RejectReason : std::uint8_t {
  none,
  invalid_side,
  invalid_time_in_force,
  invalid_handle,
  zero_quantity,
  quantity_too_large,
  price_out_of_domain,
  insufficient_trade_capacity,
  order_capacity_exhausted,
  fok_not_fillable,
  post_only_would_cross,
  self_trade_prevented,
  invalid_display_quantity,
};

enum class SelfTradePolicy : std::uint8_t {
  none,
  cancel_taker,
};

enum class AllocationMode : std::uint8_t {
  fifo,
  threshold_pro_rata,
};

struct SubmitResult {
  RejectReason reject_reason;
  Quantity executed_quantity;
  Quantity unfilled_quantity;
  std::uint32_t trade_count;
  Handle resting_handle;
};

struct StopActivation {
  OrderId order_id{0U};
  Quantity executed_quantity{0U};
  Quantity unfilled_quantity{0U};
  Handle resting_handle{};

  constexpr bool operator==(const StopActivation&) const noexcept = default;
};

enum class CancelReason : std::uint8_t {
  none,
  invalid_handle,
};

struct CancelResult {
  CancelReason reject_reason;
  OrderId order_id;
  Quantity canceled_quantity;

  constexpr bool operator==(const CancelResult&) const noexcept = default;
};

enum class AmendReason : std::uint8_t {
  none,
  invalid_handle,
  zero_quantity,
  quantity_too_large,
  increase_not_allowed,
};

struct AmendResult {
  AmendReason reject_reason;
  OrderId order_id;
  Quantity previous_quantity;
  Quantity new_quantity;
  Handle handle;

  constexpr bool operator==(const AmendResult&) const noexcept = default;
};

enum class InvariantViolation : std::uint8_t {
  none,
  bitmap_hierarchy_inconsistent,
  occupancy_mismatch,
  empty_level_metadata,
  nonempty_invalid_head,
  nonempty_invalid_tail,
  order_index_out_of_range,
  dead_order_reachable,
  head_prev_not_invalid,
  tail_next_not_invalid,
  previous_not_reciprocal,
  next_not_reciprocal,
  duplicate_order_reachable,
  order_side_mismatch,
  order_level_mismatch,
  nonpositive_remaining,
  invalid_display_state,
  aggregate_overflow,
  level_tail_mismatch,
  level_count_mismatch,
  level_aggregate_mismatch,
  live_order_unreachable,
  reachable_count_mismatch,
  invalid_stop_state,
  crossed_book,
};

struct InvariantResult {
  InvariantViolation violation{InvariantViolation::none};
  Side side{Side::buy};
  std::uint32_t level_index{kInvalidIndex};
  std::uint32_t order_index{kInvalidIndex};
  std::uint32_t reachable_count{};

  constexpr bool operator==(const InvariantResult&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_trivially_copyable_v<OrderInfo>);
static_assert(std::is_trivially_copyable_v<SubmitResult>);
static_assert(std::is_trivially_copyable_v<StopActivation>);
static_assert(std::is_trivially_copyable_v<CancelResult>);
static_assert(std::is_trivially_copyable_v<AmendResult>);
static_assert(std::is_trivially_copyable_v<InvariantResult>);

// OrderBook has one owning matching thread. Duplicate OrderId checks belong at
// the gateway; the matcher deliberately carries no hot-path identifier map.
class OrderBook {
public:
  OrderBook(PriceDomain domain, std::size_t max_orders, Quantity max_order_quantity,
            SelfTradePolicy self_trade_policy = SelfTradePolicy::none,
            AllocationMode allocation_mode = AllocationMode::fifo,
            Quantity pro_rata_minimum = Quantity{2U});

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  [[nodiscard]] SubmitResult submit_limit(OrderId id, Side side, Price price, Quantity quantity,
                                          std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_limit(OrderId id, TraderId trader_id, Side side, Price price,
                                          Quantity quantity, std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_limit(OrderId id, Side side, Price price, Quantity quantity,
                                          TimeInForce time_in_force,
                                          std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_limit(OrderId id, TraderId trader_id, Side side, Price price,
                                          Quantity quantity, TimeInForce time_in_force,
                                          std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_post_only(OrderId id, Side side, Price price,
                                              Quantity quantity, std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_iceberg(OrderId id, Side side, Price price, Quantity quantity,
                                            Quantity display_quantity,
                                            std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_iceberg(OrderId id, TraderId trader_id, Side side, Price price,
                                            Quantity quantity, Quantity display_quantity,
                                            std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_market(OrderId id, Side side, Quantity quantity,
                                           std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_stop(OrderId id, Side side, Price trigger_price,
                                         Quantity quantity, std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_stop_limit(OrderId id, Side side, Price trigger_price,
                                               Price limit_price, Quantity quantity,
                                               std::span<Trade> trades) noexcept;
  [[nodiscard]] CancelResult cancel(Handle handle) noexcept;
  [[nodiscard]] AmendResult amend_quantity(Handle handle, Quantity new_remaining) noexcept;
  [[nodiscard]] SubmitResult replace(Handle handle, Price new_price, Quantity new_quantity,
                                     std::span<Trade> trades) noexcept;
  [[nodiscard]] RejectReason preflight_limit(Side side, Price price, Quantity quantity,
                                             TimeInForce time_in_force) const noexcept;
  [[nodiscard]] RejectReason preflight_post_only(Side side, Price price,
                                                 Quantity quantity) const noexcept;
  [[nodiscard]] RejectReason preflight_market(Side side, Quantity quantity) const noexcept;
  [[nodiscard]] RejectReason preflight_stop(Side side, std::optional<Price> limit_price,
                                            Quantity quantity) const noexcept;
  [[nodiscard]] RejectReason preflight_replace(Handle handle, Price new_price,
                                               Quantity new_quantity) const noexcept;

  [[nodiscard]] std::optional<Price> best_bid() const noexcept;
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;
  [[nodiscard]] std::optional<LevelInfo> level_info(Side side, Price price) const noexcept;
  [[nodiscard]] std::optional<OrderInfo> order_info(Handle handle) const noexcept;
  [[nodiscard]] std::optional<Price> last_execution_price() const noexcept {
    return last_execution_price_;
  }
  [[nodiscard]] std::size_t required_trade_capacity() const noexcept;
  [[nodiscard]] std::span<const StopActivation> stop_activations() const noexcept {
    return {stop_activations_.get(), stop_activation_count_};
  }
  [[nodiscard]] std::size_t dormant_stop_count() const noexcept {
    return dormant_stop_count_;
  }
  [[nodiscard]] AllocationMode allocation_mode() const noexcept {
    return allocation_mode_;
  }
  [[nodiscard]] Quantity pro_rata_minimum() const noexcept {
    return Quantity{pro_rata_minimum_};
  }
  [[nodiscard]] InvariantResult check_invariants() noexcept;

private:
  friend class detail::SnapshotCodec;
  [[nodiscard]] static PriceDomain checked_domain(PriceDomain domain);
  [[nodiscard]] static std::uint32_t checked_max_quantity(Quantity quantity);
  [[nodiscard]] static std::uint32_t checked_pro_rata_minimum(AllocationMode allocation_mode,
                                                              Quantity minimum,
                                                              std::uint32_t maximum);
  [[nodiscard]] RejectReason preflight(Side side, Quantity quantity) const noexcept;
  [[nodiscard]] bool can_fully_fill(Side side, std::uint32_t limit_index,
                                    std::uint64_t quantity) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> next_level(Side side,
                                                        std::uint32_t current) const noexcept;
  [[nodiscard]] bool has_crossing_order(Side side, std::uint32_t limit_index) const noexcept;
  [[nodiscard]] bool would_self_trade(TraderId trader_id, Side side,
                                      std::uint32_t limit_index) const noexcept;
  void match(OrderId taker_id, TraderId taker_trader_id, Side taker_side,
             std::optional<std::uint32_t> limit_index,
             std::uint64_t& remaining, std::span<Trade> trades,
             std::uint32_t& trade_count) noexcept;
  void match_level(OrderId taker_id, TraderId taker_trader_id, Side taker_side,
                   std::uint32_t level_index,
                   std::uint64_t& remaining, std::span<Trade> trades,
                   std::uint32_t& trade_count) noexcept;
  void match_level_fifo(OrderId taker_id, TraderId taker_trader_id, Side taker_side,
                        std::uint32_t level_index, std::uint64_t& remaining,
                        std::span<Trade> trades, std::uint32_t& trade_count) noexcept;
  void match_level_threshold_pro_rata(OrderId taker_id, TraderId taker_trader_id,
                                      Side taker_side, std::uint32_t level_index,
                                      std::uint64_t& remaining, std::span<Trade> trades,
                                      std::uint32_t& trade_count) noexcept;
  void execute_match(OrderId taker_id, Side taker_side, std::uint32_t level_index,
                     std::uint32_t maker_index, std::uint64_t execution,
                     std::uint64_t& remaining, std::span<Trade> trades,
                     std::uint32_t& trade_count) noexcept;
  [[nodiscard]] bool stop_is_triggered(Side side, Price trigger_price) const noexcept;
  [[nodiscard]] Handle rest_stop(OrderId id, Side side, Price trigger_price,
                                 std::optional<Price> limit_price,
                                 Quantity quantity) noexcept;
  void detach_stop(std::uint32_t index) noexcept;
  void unlink_stop(Handle handle) noexcept;
  [[nodiscard]] SubmitResult activate_stop(Handle handle, std::span<Trade> trades,
                                           std::uint32_t& trade_count) noexcept;
  void trigger_stops(std::span<Trade> trades, std::uint32_t& trade_count) noexcept;
  void reset_stop_activations() noexcept {
    stop_activation_count_ = 0U;
  }
  [[nodiscard]] Handle rest(OrderId id, TraderId trader_id, Side side, std::uint32_t level_index,
                            std::uint64_t remaining, std::uint64_t display_quantity) noexcept;
  void rest_existing(Handle handle, OrderId id, TraderId trader_id, Side side,
                     std::uint32_t level_index, std::uint64_t remaining,
                     std::uint64_t display_quantity) noexcept;
  void move_to_tail(std::uint32_t order_index) noexcept;
  void unlink(Handle handle, std::uint64_t aggregate_reduction) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> best_index(Side side) const noexcept;
  [[nodiscard]] PriceLevel& level(Side side, std::uint32_t index) noexcept;
  [[nodiscard]] const PriceLevel& level(Side side, std::uint32_t index) const noexcept;
  [[nodiscard]] HierarchicalBitmap& occupancy(Side side) noexcept;
  [[nodiscard]] const HierarchicalBitmap& occupancy(Side side) const noexcept;
  [[nodiscard]] InvariantResult check_side_invariants(Side side, std::uint32_t epoch,
                                                      std::uint32_t& reachable_count) noexcept;
  [[nodiscard]] static InvariantResult invariant_failure(InvariantViolation violation, Side side,
                                                         std::uint32_t level_index,
                                                         std::uint32_t order_index,
                                                         std::uint32_t reachable_count) noexcept;

  PriceDomain domain_;
  std::uint32_t max_order_quantity_;
  std::unique_ptr<PriceLevel[]> bids_;
  std::unique_ptr<PriceLevel[]> asks_;
  HierarchicalBitmap bid_occupancy_;
  HierarchicalBitmap ask_occupancy_;
  OrderArena arena_;
  std::unique_ptr<TraderId[]> trader_ids_;
  std::unique_ptr<std::uint64_t[]> display_quantities_;
  std::unique_ptr<std::uint64_t[]> displayed_remaining_;
  std::unique_ptr<std::int64_t[]> stop_trigger_prices_;
  std::unique_ptr<std::int64_t[]> stop_limit_prices_;
  std::unique_ptr<std::uint32_t[]> stop_prev_indices_;
  std::unique_ptr<std::uint32_t[]> stop_next_indices_;
  std::uint32_t stop_head_{kInvalidIndex};
  std::uint32_t stop_tail_{kInvalidIndex};
  std::size_t dormant_stop_count_{};
  std::unique_ptr<StopActivation[]> stop_activations_;
  std::size_t stop_activation_count_{};
  std::optional<Price> last_execution_price_;
  SelfTradePolicy self_trade_policy_;
  AllocationMode allocation_mode_;
  std::uint32_t pro_rata_minimum_;
  std::unique_ptr<std::uint32_t[]> trade_report_indices_;
  std::unique_ptr<std::uint32_t[]> trade_report_epochs_;
  std::uint32_t trade_report_epoch_{};
  std::unique_ptr<std::uint32_t[]> visit_marks_;
  std::uint32_t visit_epoch_{};
};

} // namespace matching_engine

#endif
