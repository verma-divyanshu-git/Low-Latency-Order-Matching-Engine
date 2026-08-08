#ifndef MATCHING_ENGINE_SPSC_QUEUE_HPP
#define MATCHING_ENGINE_SPSC_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace matching_engine {

inline constexpr std::size_t kMaximumSpscCapacity = 1'000'000U;
inline constexpr std::size_t kSpscCachelineAlignment = 128U;

template <typename T>
  requires(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T> &&
           !std::is_pointer_v<T>)
class SpscQueue {
public:
  class Producer {
  public:
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    Producer(Producer&& other) noexcept : queue_{std::exchange(other.queue_, nullptr)} {}
    Producer& operator=(Producer&& other) noexcept {
      queue_ = std::exchange(other.queue_, nullptr);
      return *this;
    }

    [[nodiscard]] bool try_push(const T& value) noexcept {
      return queue_ != nullptr && queue_->try_push(value);
    }
    [[nodiscard]] std::size_t available() noexcept {
      return queue_ == nullptr ? 0U : queue_->producer_available();
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
      return queue_ == nullptr ? 0U : queue_->capacity_;
    }

  private:
    friend class SpscQueue;
    explicit Producer(SpscQueue& queue) noexcept : queue_{&queue} {}
    SpscQueue* queue_{};
  };

  class Consumer {
  public:
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;
    Consumer(Consumer&& other) noexcept : queue_{std::exchange(other.queue_, nullptr)} {}
    Consumer& operator=(Consumer&& other) noexcept {
      queue_ = std::exchange(other.queue_, nullptr);
      return *this;
    }

    [[nodiscard]] bool try_peek(T& value) noexcept {
      return queue_ != nullptr && queue_->try_peek(value);
    }
    [[nodiscard]] bool try_pop(T& value) noexcept {
      return queue_ != nullptr && queue_->try_pop(value);
    }

  private:
    friend class SpscQueue;
    explicit Consumer(SpscQueue& queue) noexcept : queue_{&queue} {}
    SpscQueue* queue_{};
  };

  explicit SpscQueue(std::size_t capacity)
      : capacity_{validate_capacity(capacity)}, slots_{std::make_unique<T[]>(capacity_)} {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  [[nodiscard]] std::optional<Producer> claim_producer() noexcept {
    if (producer_claimed_) {
      return std::nullopt;
    }
    producer_claimed_ = true;
    return Producer{*this};
  }

  [[nodiscard]] std::optional<Consumer> claim_consumer() noexcept {
    if (consumer_claimed_) {
      return std::nullopt;
    }
    consumer_claimed_ = true;
    return Consumer{*this};
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

  // An observational snapshot only. Concurrent activity may make the answer stale
  // immediately, but acquire loads ensure a reported non-empty slot is published.
  [[nodiscard]] bool empty() const noexcept {
    return consumer_.published_head.load(std::memory_order_acquire) ==
           producer_.published_tail.load(std::memory_order_acquire);
  }

#if defined(MATCHING_ENGINE_TEST_SEAMS)
  void set_empty_index_for_testing(std::uint64_t index) noexcept {
    if (producer_claimed_ || consumer_claimed_ || !empty()) {
      return;
    }
    producer_.tail = index;
    producer_.cached_head = index;
    producer_.published_tail.store(index, std::memory_order_relaxed);
    consumer_.head = index;
    consumer_.cached_tail = index;
    consumer_.published_head.store(index, std::memory_order_relaxed);
  }
#endif

private:
  struct alignas(kSpscCachelineAlignment) ProducerState {
    std::uint64_t tail{};
    std::uint64_t cached_head{};
    std::atomic<std::uint64_t> published_tail{};
  };

  struct alignas(kSpscCachelineAlignment) ConsumerState {
    std::uint64_t head{};
    std::uint64_t cached_tail{};
    std::atomic<std::uint64_t> published_head{};
  };

  [[nodiscard]] static std::size_t validate_capacity(std::size_t capacity) {
    if (capacity == 0U || capacity > kMaximumSpscCapacity ||
        capacity > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::invalid_argument{"invalid SPSC queue capacity"};
    }
    return capacity;
  }

  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::uint64_t tail = producer_.tail;
    if (tail - producer_.cached_head == capacity_) {
      producer_.cached_head = consumer_.published_head.load(std::memory_order_acquire);
      if (tail - producer_.cached_head == capacity_) {
        return false;
      }
    }

    slots_[static_cast<std::size_t>(tail % capacity_)] = value;
    producer_.tail = tail + 1U;
    // Payload initialization happens-before a consumer that observes this tail.
    producer_.published_tail.store(producer_.tail, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_peek(T& value) noexcept {
    const std::uint64_t head = consumer_.head;
    if (head == consumer_.cached_tail) {
      // Acquire pairs with producer release publication before reading payload.
      consumer_.cached_tail = producer_.published_tail.load(std::memory_order_acquire);
      if (head == consumer_.cached_tail) {
        return false;
      }
    }
    value = slots_[static_cast<std::size_t>(head % capacity_)];
    return true;
  }

  [[nodiscard]] bool try_pop(T& value) noexcept {
    if (!try_peek(value)) {
      return false;
    }
    ++consumer_.head;
    // Payload read happens-before producer reuse after its acquire head load.
    consumer_.published_head.store(consumer_.head, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t producer_available() noexcept {
    producer_.cached_head = consumer_.published_head.load(std::memory_order_acquire);
    return capacity_ - static_cast<std::size_t>(producer_.tail - producer_.cached_head);
  }

  std::size_t capacity_;
  std::unique_ptr<T[]> slots_;
  ProducerState producer_{};
  ConsumerState consumer_{};
  bool producer_claimed_{};
  bool consumer_claimed_{};
};

} // namespace matching_engine

#endif
