#include "matching_engine/pipeline.hpp"

#include <stdexcept>

namespace matching_engine {

DurableIngressStage::DurableIngressStage(Sequencer sequencer, MmapJournal journal,
                                         CommandQueue::Producer producer) noexcept
    : sequencer_{sequencer}, journal_{std::move(journal)}, producer_{std::move(producer)} {}

IngressStatus DurableIngressStage::try_ingest(const CommandPayload& payload,
                                              std::uint64_t logical_time) noexcept {
  if (stopped_) {
    return IngressStatus::stopped_poisoned;
  }
  if (recovery_required_ || !journal_.writable()) {
    recovery_required_ = true;
    return IngressStatus::recovery_required;
  }
  if (validate_command_payload(payload) != CommandValidationError::none) {
    return IngressStatus::invalid_payload;
  }
  if (sequencer_.exhausted()) {
    return IngressStatus::sequence_exhausted;
  }
  if (logical_time < sequencer_.last_logical_time()) {
    return IngressStatus::decreasing_logical_time;
  }
  if (producer_.available() == 0U) {
    return IngressStatus::queue_backpressure;
  }
  if (journal_.full()) {
    return IngressStatus::journal_full;
  }

  const auto command = sequencer_.stamp(payload, logical_time);
  if (!command.has_value()) {
    switch (command.error()) {
    case SequencerError::invalid_payload:
      return IngressStatus::invalid_payload;
    case SequencerError::decreasing_logical_time:
      return IngressStatus::decreasing_logical_time;
    case SequencerError::sequence_exhausted:
      return IngressStatus::sequence_exhausted;
    }
    stopped_ = true;
    return IngressStatus::stopped_poisoned;
  }

  // Durability always precedes visibility to the matcher. Any failure after
  // stamping stops this ingress instance so it cannot continue across a gap.
  last_journal_error_ = journal_.append(*command);
  if (last_journal_error_ != JournalError::none) {
    recovery_required_ = true;
    return last_journal_error_ == JournalError::commit_indeterminate
               ? IngressStatus::commit_indeterminate
               : IngressStatus::persistence_failure;
  }
  if (!producer_.try_push(*command)) {
    stopped_ = true;
    return IngressStatus::stopped_poisoned;
  }
  return IngressStatus::progress;
}

MatchingStage::MatchingStage(std::unique_ptr<SequencedEngine> engine,
                             CommandQueue::Consumer commands, EventQueue::Producer events)
    : engine_{std::move(engine)}, commands_{std::move(commands)}, events_{std::move(events)} {
  if (engine_ == nullptr) {
    throw std::invalid_argument{"matching stage requires an engine"};
  }
  scratch_capacity_ = engine_->maximum_event_capacity();
  if (events_.capacity() < scratch_capacity_) {
    throw std::invalid_argument{"event queue cannot hold maximum engine event batch"};
  }
  scratch_ = std::make_unique<EngineEvent[]>(scratch_capacity_);
}

MatchingStatus MatchingStage::process_one() noexcept {
  if (poisoned_) {
    return MatchingStatus::poisoned;
  }

  SequencedCommand command{};
  if (!commands_.try_peek(command)) {
    return MatchingStatus::input_empty;
  }
  const auto poison = [this](MatchingStatus status) noexcept {
    poisoned_ = true;
    return status;
  };
  if (engine_->sequence_exhausted()) {
    return poison(MatchingStatus::sequence_exhausted);
  }
  if (validate_command_payload(command.payload) != CommandValidationError::none) {
    return poison(MatchingStatus::invalid_command);
  }
  if (command.sequence != engine_->next_sequence()) {
    return poison(MatchingStatus::invalid_sequence);
  }
  if (command.logical_time < engine_->last_logical_time()) {
    return poison(MatchingStatus::decreasing_logical_time);
  }
  const std::size_t required = engine_->required_event_capacity(command.payload);
  if (required == 0U || required > scratch_capacity_ || required > events_.capacity()) {
    return poison(MatchingStatus::impossible_event_capacity);
  }
  if (events_.available() < required) {
    return MatchingStatus::output_backpressure;
  }

  const ApplyResult result =
      engine_->apply(command, std::span<EngineEvent>{scratch_.get(), scratch_capacity_});
  if (result.status != ApplyStatus::applied) {
    switch (result.status) {
    case ApplyStatus::invalid_command:
      return poison(MatchingStatus::invalid_command);
    case ApplyStatus::invalid_sequence:
      return poison(MatchingStatus::invalid_sequence);
    case ApplyStatus::decreasing_logical_time:
      return poison(MatchingStatus::decreasing_logical_time);
    case ApplyStatus::sequence_exhausted:
      return poison(MatchingStatus::sequence_exhausted);
    case ApplyStatus::insufficient_event_capacity:
      return poison(MatchingStatus::impossible_event_capacity);
    case ApplyStatus::applied:
      break;
    }
  }
  if (result.event_count == 0U || result.event_count > required) {
    return poison(MatchingStatus::internal_invariant_failure);
  }
  if (!events_.try_push_batch(std::span<const EngineEvent>{scratch_.get(), result.event_count})) {
    // Capacity was preflighted and this is the sole producer. Batch insertion
    // therefore cannot fail unless queue ownership or an invariant is broken.
    // The input remains retained so recovery cannot silently skip the command.
    return poison(MatchingStatus::internal_invariant_failure);
  }

  bbo_snapshot_.publish(BboState{.bid_price = engine_->order_book().best_bid(),
                                 .bid_quantity = engine_->order_book()
                                                     .best_bid()
                                                     .transform([this](Price price) {
                                                       return engine_->order_book()
                                                           .level_info(Side::buy, price)
                                                           ->aggregate_quantity;
                                                     })
                                                     .value_or(Quantity{0U}),
                                 .ask_price = engine_->order_book().best_ask(),
                                 .ask_quantity = engine_->order_book()
                                                     .best_ask()
                                                     .transform([this](Price price) {
                                                       return engine_->order_book()
                                                           .level_info(Side::sell, price)
                                                           ->aggregate_quantity;
                                                     })
                                                     .value_or(Quantity{0U})});

  SequencedCommand released{};
  if (!commands_.try_pop(released) || released != command) {
    return poison(MatchingStatus::internal_invariant_failure);
  }
  return MatchingStatus::progress;
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
