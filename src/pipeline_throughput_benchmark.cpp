#include "matching_engine/pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

int run_benchmark() {
  using namespace matching_engine;
  constexpr std::size_t operations = 1'000U;
  constexpr std::size_t queue_capacity = 256U;
  const std::filesystem::path journal_path =
      std::filesystem::temp_directory_path() /
      ("matching-engine-pipeline-benchmark-" +
       std::to_string(static_cast<std::uint64_t>(::getpid())) + ".journal");
  std::error_code cleanup_error;
  std::filesystem::remove(journal_path, cleanup_error);
  auto journal = MmapJournal::create(journal_path, operations);
  if (!journal.has_value()) {
    return 1;
  }

  CommandQueue commands{queue_capacity};
  EventQueue events{queue_capacity};
  auto command_producer = commands.claim_producer();
  auto command_consumer = commands.claim_consumer();
  auto event_producer = events.claim_producer();
  auto event_consumer = events.claim_consumer();
  if (!command_producer || !command_consumer || !event_producer || !event_consumer) {
    return 2;
  }
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*command_producer)};
  MatchingStage matching{
      std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 1U}, 0U, Quantity{1U}),
      std::move(*command_consumer), std::move(*event_producer)};
  PublicationStage publication{std::move(*event_consumer)};
  std::atomic<bool> ingress_done{};
  std::atomic<bool> matching_done{};
  std::atomic<bool> completed{};
  std::atomic<bool> failed{};
  std::stop_source stop;
  std::uint64_t event_count{};
  std::uint64_t checksum{};
  const auto fail = [&] {
    failed.store(true, std::memory_order_release);
    stop.request_stop();
  };
  std::jthread watchdog{[&](const std::stop_token& token) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!token.stop_requested() && !completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!completed.load(std::memory_order_acquire) && !token.stop_requested()) {
      fail();
    }
  }};

  const auto start = std::chrono::steady_clock::now();
  std::jthread ingress_thread{[&] {
    for (std::size_t index = 0U; index < operations && !stop.stop_requested();) {
      const IngressStatus status = ingress.try_ingest(
          CommandPayload::submit_market(OrderId{index + 1U}, Side::buy, Quantity{1U}), index + 1U);
      if (status == IngressStatus::progress) {
        ++index;
      } else if (status != IngressStatus::queue_backpressure) {
        fail();
      } else {
        std::this_thread::yield();
      }
    }
    ingress_done.store(true, std::memory_order_release);
  }};
  std::jthread matching_thread{[&] {
    while ((!ingress_done.load(std::memory_order_acquire) || !commands.empty()) &&
           !stop.stop_requested()) {
      const MatchingStatus status = matching.process_one();
      if (status != MatchingStatus::progress && status != MatchingStatus::input_empty &&
          status != MatchingStatus::output_backpressure) {
        fail();
      }
      if (status != MatchingStatus::progress) {
        std::this_thread::yield();
      }
    }
    matching_done.store(true, std::memory_order_release);
  }};
  std::jthread publication_thread{[&] {
    while (!matching_done.load(std::memory_order_acquire) || !events.empty()) {
      EngineEvent event{};
      if (publication.try_pop(event) == PublicationStatus::progress) {
        ++event_count;
        checksum += event.command_sequence.value();
      } else if (stop.stop_requested() && matching_done.load(std::memory_order_acquire)) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
  }};
  ingress_thread.join();
  matching_thread.join();
  publication_thread.join();
  completed.store(true, std::memory_order_release);
  watchdog.request_stop();
  watchdog.join();
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count());
  const double throughput = elapsed == 0U ? 0.0
                                          : static_cast<double>(operations) * 1'000'000'000.0 /
                                                static_cast<double>(elapsed);
  std::filesystem::remove(journal_path, cleanup_error);

  std::cout << "{\"benchmark\":\"durable_three_stage_pipeline\",\"operations\":" << operations
            << ",\"command_queue_capacity\":" << queue_capacity
            << ",\"event_queue_capacity\":" << queue_capacity
            << ",\"workload\":\"rejected_market_no_liquidity\",\"persistence\":\"mmap_msync_"
               "fsync_per_command\",\"elapsed_ns\":"
            << elapsed << ",\"commands_per_second\":" << throughput
            << ",\"event_count\":" << event_count << ",\"checksum\":" << checksum
            << ",\"thread_pinning\":\"external_or_none\"}\n";
  return !failed.load(std::memory_order_acquire) && event_count == operations ? 0 : 3;
}

int main() {
  try {
    return run_benchmark();
  } catch (const std::exception& exception) {
    std::cerr << "pipeline_throughput_benchmark: " << exception.what() << '\n';
  } catch (...) {
    std::cerr << "pipeline_throughput_benchmark: unknown failure\n";
  }
  return 1;
}
