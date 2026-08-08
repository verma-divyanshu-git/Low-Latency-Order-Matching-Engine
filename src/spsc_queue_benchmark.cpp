#include "matching_engine/spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stop_token>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kOperations = 10'000'000U;
constexpr std::size_t kCapacity = 1024U;

template <typename Work> std::uint64_t elapsed_ns(Work&& work) {
  const auto start = Clock::now();
  work();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

double operations_per_second(std::uint64_t operations, std::uint64_t nanoseconds) {
  return nanoseconds == 0U
             ? 0.0
             : static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(nanoseconds);
}

} // namespace

int run_benchmark() {
  matching_engine::SpscQueue<std::uint64_t> loop_queue{kCapacity};
  auto loop_producer = loop_queue.claim_producer();
  auto loop_consumer = loop_queue.claim_consumer();
  if (!loop_producer || !loop_consumer) {
    return 1;
  }
  std::uint64_t loop_checksum{};
  const std::uint64_t loop_ns = elapsed_ns([&] {
    for (std::uint64_t value = 0U; value < kOperations; ++value) {
      static_cast<void>(loop_producer->try_push(value));
      std::uint64_t result{};
      static_cast<void>(loop_consumer->try_pop(result));
      loop_checksum += result;
    }
  });

  matching_engine::SpscQueue<std::uint64_t> handoff_queue{kCapacity};
  auto handoff_producer = handoff_queue.claim_producer();
  auto handoff_consumer = handoff_queue.claim_consumer();
  if (!handoff_producer || !handoff_consumer) {
    return 1;
  }
  std::atomic<bool> start{};
  std::atomic<bool> completed{};
  std::atomic<bool> failed{};
  std::stop_source stop;
  std::uint64_t handoff_checksum{};
  std::jthread watchdog{[&](const std::stop_token& token) {
    const auto deadline = Clock::now() + std::chrono::seconds{30};
    while (!token.stop_requested() && !completed.load(std::memory_order_acquire) &&
           Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!completed.load(std::memory_order_acquire) && !token.stop_requested()) {
      failed.store(true, std::memory_order_release);
      stop.request_stop();
    }
  }};
  const std::uint64_t handoff_ns = elapsed_ns([&] {
    std::jthread producer{[endpoint = std::move(*handoff_producer), &start, &stop]() mutable {
      while (!start.load(std::memory_order_acquire) && !stop.stop_requested()) {
        std::this_thread::yield();
      }
      for (std::uint64_t value = 0U; value < kOperations && !stop.stop_requested(); ++value) {
        while (!endpoint.try_push(value) && !stop.stop_requested()) {
          std::this_thread::yield();
        }
      }
    }};
    std::jthread consumer{[endpoint = std::move(*handoff_consumer), &start, &stop, &failed,
                           &handoff_checksum]() mutable {
      start.store(true, std::memory_order_release);
      for (std::uint64_t index = 0U; index < kOperations && !stop.stop_requested(); ++index) {
        std::uint64_t value{};
        while (!endpoint.try_pop(value) && !stop.stop_requested()) {
          std::this_thread::yield();
        }
        if (stop.stop_requested()) {
          break;
        }
        if (value != index) {
          failed.store(true, std::memory_order_release);
          stop.request_stop();
          break;
        }
        handoff_checksum += value;
      }
    }};
  });
  completed.store(true, std::memory_order_release);
  watchdog.request_stop();
  watchdog.join();

  std::cout << "{\"benchmark\":\"spsc_queue\",\"operations\":" << kOperations
            << ",\"capacity\":" << kCapacity << ",\"single_thread_loop_ns\":" << loop_ns
            << ",\"single_thread_loop_ops_per_second\":"
            << operations_per_second(kOperations, loop_ns)
            << ",\"contention_handoff_ns\":" << handoff_ns
            << ",\"contention_handoff_ops_per_second\":"
            << operations_per_second(kOperations, handoff_ns)
            << ",\"loop_checksum\":" << loop_checksum
            << ",\"handoff_checksum\":" << handoff_checksum
            << ",\"thread_pinning\":\"external_or_none\"}\n";
  return !failed.load(std::memory_order_acquire) && loop_checksum == handoff_checksum ? 0 : 2;
}

int main() {
  try {
    return run_benchmark();
  } catch (const std::exception& exception) {
    std::cerr << "spsc_queue_benchmark: " << exception.what() << '\n';
  } catch (...) {
    std::cerr << "spsc_queue_benchmark: unknown failure\n";
  }
  return 1;
}
