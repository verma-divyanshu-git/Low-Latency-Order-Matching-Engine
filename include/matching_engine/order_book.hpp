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

struct Trade {
  OrderId buy_id{0U};
  OrderId sell_id{0U};
  Price price{0};
  Quantity quantity{0U};

  constexpr bool operator==(const Trade&) const noexcept = default;
};

enum class RejectReason : std::uint8_t {
  none,
  zero_quantity,
  quantity_too_large,
  price_out_of_domain,
  insufficient_trade_capacity,
  order_capacity_exhausted,
};

struct SubmitResult {
  RejectReason reject_reason;
  Quantity executed_quantity;
  Quantity unfilled_quantity;
  std::uint32_t trade_count;
  Handle resting_handle;
};

static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_trivially_copyable_v<SubmitResult>);

// OrderBook has one owning matching thread. Duplicate OrderId checks belong at
// the gateway; the matcher deliberately carries no hot-path identifier map.
class OrderBook {
public:
  OrderBook(PriceDomain domain, std::size_t max_orders, Quantity max_order_quantity);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  [[nodiscard]] SubmitResult submit_limit(OrderId id, Side side, Price price, Quantity quantity,
                                          std::span<Trade> trades) noexcept;
  [[nodiscard]] SubmitResult submit_market(OrderId id, Side side, Quantity quantity,
                                           std::span<Trade> trades) noexcept;

  [[nodiscard]] std::optional<Price> best_bid() const noexcept;
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;
  [[nodiscard]] std::optional<LevelInfo> level_info(Side side, Price price) const noexcept;
  [[nodiscard]] std::size_t required_trade_capacity() const noexcept;

private:
  [[nodiscard]] static PriceDomain checked_domain(PriceDomain domain);
  [[nodiscard]] static std::uint32_t checked_max_quantity(Quantity quantity);
  [[nodiscard]] SubmitResult validate(Quantity quantity, std::span<Trade> trades) const noexcept;
  [[nodiscard]] bool has_crossing_order(Side side, std::uint32_t limit_index) const noexcept;
  void match(OrderId taker_id, Side taker_side, std::optional<std::uint32_t> limit_index,
             std::uint64_t& remaining, std::span<Trade> trades,
             std::uint32_t& trade_count) noexcept;
  void match_level(OrderId taker_id, Side taker_side, std::uint32_t level_index,
                   std::uint64_t& remaining, std::span<Trade> trades,
                   std::uint32_t& trade_count) noexcept;
  [[nodiscard]] Handle rest(OrderId id, Side side, std::uint32_t level_index,
                            std::uint64_t remaining) noexcept;
  void remove_head(Side side, std::uint32_t level_index) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> best_index(Side side) const noexcept;
  [[nodiscard]] PriceLevel& level(Side side, std::uint32_t index) noexcept;
  [[nodiscard]] const PriceLevel& level(Side side, std::uint32_t index) const noexcept;
  [[nodiscard]] HierarchicalBitmap& occupancy(Side side) noexcept;
  [[nodiscard]] const HierarchicalBitmap& occupancy(Side side) const noexcept;

  PriceDomain domain_;
  std::uint32_t max_order_quantity_;
  std::unique_ptr<PriceLevel[]> bids_;
  std::unique_ptr<PriceLevel[]> asks_;
  HierarchicalBitmap bid_occupancy_;
  HierarchicalBitmap ask_occupancy_;
  OrderArena arena_;
};

} // namespace matching_engine

#endif
