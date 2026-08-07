#ifndef MATCHING_ENGINE_ORDER_ARENA_HPP
#define MATCHING_ENGINE_ORDER_ARENA_HPP

#include "matching_engine/order.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace matching_engine {

enum class ArenaError : std::uint8_t {
  none,
  full,
  invalid_handle,
};

struct AcquireResult {
  Handle handle;
  ArenaError error;
};

class OrderArena {
public:
  explicit OrderArena(std::uint32_t capacity)
      : slots_(capacity, empty_order()), generations_(capacity, 1U),
        free_next_(capacity, kInvalidIndex), live_(capacity, 0U), capacity_(capacity) {
    for (std::uint32_t index = 0; index < capacity_; ++index) {
      free_next_[index] = index + 1U < capacity_ ? index + 1U : kInvalidIndex;
    }
    if (capacity_ != 0U) {
      free_head_ = 0U;
    }
  }

  OrderArena(const OrderArena&) = delete;
  OrderArena& operator=(const OrderArena&) = delete;
  OrderArena(OrderArena&&) = delete;
  OrderArena& operator=(OrderArena&&) = delete;

  [[nodiscard]] AcquireResult acquire(const Order& order) noexcept {
    if (free_head_ == kInvalidIndex) {
      return {.handle = {.index = kInvalidIndex, .generation = 0U}, .error = ArenaError::full};
    }

    const std::uint32_t index = free_head_;
    free_head_ = free_next_[index];
    slots_[index] = order;
    live_[index] = 1U;
    ++size_;
    return {.handle = {.index = index, .generation = generations_[index]},
            .error = ArenaError::none};
  }

  [[nodiscard]] Order* resolve(Handle handle) noexcept {
    if (!is_live_handle(handle)) {
      return nullptr;
    }
    return &slots_[handle.index];
  }

  [[nodiscard]] ArenaError release(Handle handle) noexcept {
    if (!is_live_handle(handle)) {
      return ArenaError::invalid_handle;
    }

    const std::uint32_t index = handle.index;
    live_[index] = 0U;
    generations_[index] = generations_[index] == std::numeric_limits<std::uint32_t>::max()
                              ? 1U
                              : generations_[index] + 1U;
    free_next_[index] = free_head_;
    free_head_ = index;
    --size_;
    return ArenaError::none;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return capacity_;
  }
  [[nodiscard]] std::uint32_t size() const noexcept {
    return size_;
  }

private:
  [[nodiscard]] static constexpr Order empty_order() noexcept {
    return {
        .id = OrderId{0U},
        .remaining = Quantity{0U},
        .prev_index = kInvalidIndex,
        .next_index = kInvalidIndex,
        .encoded_level_side = 0U,
        .reserved_flags = 0U,
    };
  }

  [[nodiscard]] bool is_live_handle(Handle handle) const noexcept {
    return handle.generation != 0U && handle.index < capacity_ && live_[handle.index] != 0U &&
           generations_[handle.index] == handle.generation;
  }

  std::vector<Order> slots_;
  std::vector<std::uint32_t> generations_;
  std::vector<std::uint32_t> free_next_;
  std::vector<std::uint8_t> live_;
  std::uint32_t capacity_;
  std::uint32_t size_{};
  std::uint32_t free_head_{kInvalidIndex};
};

} // namespace matching_engine

#endif
