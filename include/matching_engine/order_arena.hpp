#ifndef MATCHING_ENGINE_ORDER_ARENA_HPP
#define MATCHING_ENGINE_ORDER_ARENA_HPP

#include "matching_engine/order.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace matching_engine {

namespace detail {

[[nodiscard]] constexpr std::uint32_t next_generation(std::uint32_t generation) noexcept {
  return generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
}

} // namespace detail

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
  explicit OrderArena(std::size_t capacity)
      : capacity_(checked_capacity(capacity)),
        slots_(capacity_ == 0U ? nullptr : std::make_unique<Slot[]>(capacity_)) {
    for (std::uint32_t index = 0; index < capacity_; ++index) {
      slots_[index].free_next = index == capacity_ - 1U ? kInvalidIndex : index + 1U;
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
    Slot& slot = slots_[index];
    free_head_ = slot.free_next;
    slot.order = order;
    slot.live = true;
    ++size_;
    return {.handle = {.index = index, .generation = slot.generation}, .error = ArenaError::none};
  }

  // The returned pointer is borrowed only until this handle is released.
  // Retaining it after release can alias a future occupant and bypass generation validation.
  [[nodiscard]] Order* resolve(Handle handle) noexcept {
    if (!is_live_handle(handle)) {
      return nullptr;
    }
    return &slots_[handle.index].order;
  }

  [[nodiscard]] ArenaError release(Handle handle) noexcept {
    if (!is_live_handle(handle)) {
      return ArenaError::invalid_handle;
    }

    const std::uint32_t index = handle.index;
    Slot& slot = slots_[index];
    slot.live = false;
    slot.generation = detail::next_generation(slot.generation);
    slot.free_next = free_head_;
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
  struct Slot {
    Order order{
        .id = OrderId{0U},
        .remaining = Quantity{0U},
        .prev_index = kInvalidIndex,
        .next_index = kInvalidIndex,
        .encoded_level_side = 0U,
        .reserved_flags = 0U,
    };
    std::uint32_t generation{1U};
    std::uint32_t free_next{kInvalidIndex};
    bool live{};
  };

  [[nodiscard]] static std::uint32_t checked_capacity(std::size_t capacity) {
    if (capacity > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error{"OrderArena capacity exceeds handle index range"};
    }
    return static_cast<std::uint32_t>(capacity);
  }

  [[nodiscard]] bool is_live_handle(Handle handle) const noexcept {
    return handle.generation != 0U && handle.index < capacity_ && slots_[handle.index].live &&
           slots_[handle.index].generation == handle.generation;
  }

  std::uint32_t capacity_;
  std::unique_ptr<Slot[]> slots_;
  std::uint32_t size_{};
  std::uint32_t free_head_{kInvalidIndex};
};

} // namespace matching_engine

#endif
