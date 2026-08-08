#include "matching_engine/spsc_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> allocation_count{};
}

void* operator new(std::size_t size) {
  allocation_count.fetch_add(1U, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
  allocation_count.fetch_add(1U, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

int main() {
  matching_engine::SpscQueue<std::uint64_t> queue{64U};
  auto producer = queue.claim_producer();
  auto consumer = queue.claim_consumer();
  if (!producer || !consumer) {
    return 1;
  }
  const std::uint64_t before = allocation_count.load(std::memory_order_relaxed);
  for (std::uint64_t value = 0U; value < 1'000'000U; value += 4U) {
    const std::array batch{value, value + 1U, value + 2U, value + 3U};
    if (!producer->try_push_batch(batch)) {
      return 2;
    }
    for (const std::uint64_t expected : batch) {
      std::uint64_t result{};
      if (!consumer->try_pop(result) || result != expected) {
        return 3;
      }
    }
  }
  return allocation_count.load(std::memory_order_relaxed) == before ? 0 : 4;
}
