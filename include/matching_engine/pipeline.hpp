#ifndef MATCHING_ENGINE_PIPELINE_HPP
#define MATCHING_ENGINE_PIPELINE_HPP

#include "matching_engine/bbo.hpp"
#include "matching_engine/journal.hpp"
#include "matching_engine/snapshot.hpp"
#include "matching_engine/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace matching_engine {

using CommandQueue = SpscQueue<SequencedCommand>;
using EventQueue = SpscQueue<EngineEvent>;

enum class IngressStatus : std::uint8_t {
  progress,
  invalid_payload,
  decreasing_logical_time,
  sequence_exhausted,
  queue_backpressure,
  journal_full,
  persistence_failure,
  commit_indeterminate,
  recovery_required,
  stopped_poisoned,
};

enum class MatchingStatus : std::uint8_t {
  progress,
  input_empty,
  output_backpressure,
  invalid_command,
  invalid_sequence,
  decreasing_logical_time,
  sequence_exhausted,
  impossible_event_capacity,
  internal_invariant_failure,
  poisoned,
};

enum class PublicationStatus : std::uint8_t {
  progress,
  input_empty,
};

class DurableIngressStage {
public:
  DurableIngressStage(Sequencer sequencer, MmapJournal journal,
                      CommandQueue::Producer producer) noexcept;

  [[nodiscard]] IngressStatus try_ingest(const CommandPayload& payload,
                                         std::uint64_t logical_time) noexcept;
  [[nodiscard]] Sequence next_sequence() const noexcept {
    return sequencer_.next_sequence();
  }
  [[nodiscard]] std::uint64_t journal_size() const noexcept {
    return journal_.size();
  }
  [[nodiscard]] JournalError last_journal_error() const noexcept {
    return last_journal_error_;
  }
  [[nodiscard]] bool poisoned() const noexcept {
    return recovery_required_ || stopped_;
  }
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
  [[nodiscard]] const void* journal_identity_for_testing() const noexcept {
    return &journal_;
  }
#endif

private:
  Sequencer sequencer_;
  MmapJournal journal_;
  CommandQueue::Producer producer_;
  JournalError last_journal_error_{JournalError::none};
  bool recovery_required_{};
  bool stopped_{};
};

class MatchingStage {
public:
  MatchingStage(std::unique_ptr<SequencedEngine> engine, CommandQueue::Consumer commands,
                EventQueue::Producer events);

  [[nodiscard]] MatchingStatus process_one() noexcept;
  [[nodiscard]] SnapshotError save_snapshot(const std::filesystem::path& path);
  [[nodiscard]] SequencedEngine& engine() noexcept {
    return *engine_;
  }
  [[nodiscard]] const SequencedEngine& engine() const noexcept {
    return *engine_;
  }
  [[nodiscard]] const BboSnapshot& bbo_snapshot() const noexcept {
    return bbo_snapshot_;
  }
  [[nodiscard]] bool poisoned() const noexcept {
    return poisoned_;
  }

private:
  std::unique_ptr<SequencedEngine> engine_;
  CommandQueue::Consumer commands_;
  EventQueue::Producer events_;
  std::unique_ptr<EngineEvent[]> scratch_;
  std::size_t scratch_capacity_{};
  BboSnapshot bbo_snapshot_{};
  bool poisoned_{};
};

class PublicationStage {
public:
  explicit PublicationStage(EventQueue::Consumer consumer) noexcept
      : consumer_{std::move(consumer)} {}

  [[nodiscard]] PublicationStatus try_pop(EngineEvent& event) noexcept {
    return consumer_.try_pop(event) ? PublicationStatus::progress : PublicationStatus::input_empty;
  }

  template <typename Callback>
    requires std::is_nothrow_invocable_v<Callback&, const EngineEvent&>
  [[nodiscard]] std::size_t drain(Callback&& callback) noexcept {
    std::size_t count{};
    EngineEvent event{};
    while (consumer_.try_pop(event)) {
      std::invoke(callback, event);
      ++count;
    }
    return count;
  }

private:
  EventQueue::Consumer consumer_;
};

} // namespace matching_engine

#endif
