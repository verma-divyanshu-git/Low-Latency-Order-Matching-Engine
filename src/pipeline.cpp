#include "matching_engine/pipeline.hpp"

#include <stdexcept>

namespace matching_engine {

DurableIngressStage::DurableIngressStage(Sequencer sequencer, MmapJournal journal,
                                         CommandQueue::Producer producer) noexcept
    : sequencer_{sequencer}, journal_{std::move(journal)}, producer_{std::move(producer)} {}

PipelineStatus DurableIngressStage::try_ingest(const CommandPayload& payload,
                                               std::uint64_t logical_time) noexcept {
  if (poisoned_ || !journal_.writable()) {
    poisoned_ = true;
    return PipelineStatus::stopped_poisoned;
  }
  if (producer_.available() == 0U) {
    return PipelineStatus::ingress_backpressure;
  }
  if (journal_.full()) {
    return PipelineStatus::journal_full;
  }

  const auto command = sequencer_.stamp(payload, logical_time);
  if (!command.has_value()) {
    if (command.error() == SequencerError::sequence_exhausted) {
      poisoned_ = true;
      return PipelineStatus::stopped_poisoned;
    }
    return PipelineStatus::invalid_command;
  }

  // Durability always precedes visibility to the matcher. Any failure after
  // stamping stops this ingress instance so it cannot continue across a gap.
  last_journal_error_ = journal_.append(*command);
  if (last_journal_error_ != JournalError::none) {
    poisoned_ = true;
    return PipelineStatus::persistence_required;
  }
  if (!producer_.try_push(*command)) {
    poisoned_ = true;
    return PipelineStatus::stopped_poisoned;
  }
  return PipelineStatus::progress;
}

MatchingStage::MatchingStage(std::unique_ptr<SequencedEngine> engine,
                             CommandQueue::Consumer commands, EventQueue::Producer events)
    : engine_{std::move(engine)}, commands_{std::move(commands)}, events_{std::move(events)} {
  if (engine_ == nullptr) {
    throw std::invalid_argument{"matching stage requires an engine"};
  }
  scratch_capacity_ = engine_->maximum_event_capacity();
  scratch_ = std::make_unique<EngineEvent[]>(scratch_capacity_);
}

PipelineStatus MatchingStage::process_one() noexcept {
  if (poisoned_) {
    return PipelineStatus::stopped_poisoned;
  }

  SequencedCommand command{};
  if (!commands_.try_peek(command)) {
    return PipelineStatus::input_empty;
  }
  const std::size_t required = engine_->required_event_capacity(command.payload);
  if (required == 0U || required > scratch_capacity_) {
    poisoned_ = true;
    return PipelineStatus::stopped_poisoned;
  }
  if (events_.available() < required) {
    return PipelineStatus::output_backpressure;
  }

  const ApplyResult result =
      engine_->apply(command, std::span<EngineEvent>{scratch_.get(), scratch_capacity_});
  if (result.status != ApplyStatus::applied || result.event_count > required) {
    poisoned_ = true;
    return result.status == ApplyStatus::invalid_command ? PipelineStatus::invalid_command
                                                         : PipelineStatus::stopped_poisoned;
  }
  for (std::size_t index = 0U; index < result.event_count; ++index) {
    if (!events_.try_push(scratch_[index])) {
      poisoned_ = true;
      return PipelineStatus::stopped_poisoned;
    }
  }

  SequencedCommand released{};
  if (!commands_.try_pop(released) || released != command) {
    poisoned_ = true;
    return PipelineStatus::stopped_poisoned;
  }
  return PipelineStatus::progress;
}

SnapshotError MatchingStage::save_snapshot(const std::filesystem::path& path) {
  if (poisoned_) {
    return SnapshotError::invalid_state;
  }
  const Sequence next = engine_->next_sequence();
  const SnapshotPoint point{
      engine_->sequence_exhausted() ? next : Sequence{next.value() - 1U},
      engine_->last_logical_time(),
  };
  // Owner-thread-only and command-boundary-only. Persistence may stall matching.
  return save_snapshot_atomic(path, *engine_, point);
}

} // namespace matching_engine
