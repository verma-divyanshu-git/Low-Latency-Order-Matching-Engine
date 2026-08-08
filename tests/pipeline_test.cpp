#include "matching_engine/pipeline.hpp"
#include "matching_engine/replay.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace matching_engine {
namespace {

class PipelineTemporaryDirectory {
public:
  PipelineTemporaryDirectory() {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("matching-engine-pipeline-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
             "-" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }
  ~PipelineTemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct PipelineFixture {
  PipelineTemporaryDirectory temporary;
  SpscQueue<SequencedCommand> commands{2U};
  SpscQueue<EngineEvent> events{8U};

  static std::unique_ptr<SequencedEngine> make_engine() {
    return std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 5U}, 3U, Quantity{100U});
  }
};

template <typename Callback>
concept PublicationDrainable = requires(PublicationStage& publication, Callback&& callback) {
  publication.drain(std::forward<Callback>(callback));
};

struct NothrowPublicationCallback {
  void operator()(const EngineEvent&) const noexcept {}
};

struct ThrowingPublicationCallback {
  void operator()(const EngineEvent&) const {}
};

static_assert(PublicationDrainable<NothrowPublicationCallback>);
static_assert(!PublicationDrainable<ThrowingPublicationCallback>);

TEST(PipelineTest, AppendsBeforePublishingAndPreservesOrderedEvents) {
  PipelineFixture fixture;
  auto journal = MmapJournal::create(fixture.temporary.path() / "commands.journal", 4U);
  ASSERT_TRUE(journal.has_value());
  auto command_producer = fixture.commands.claim_producer();
  auto command_consumer = fixture.commands.claim_consumer();
  auto event_producer = fixture.events.claim_producer();
  auto event_consumer = fixture.events.claim_consumer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer && event_consumer);

  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*command_producer)};
  MatchingStage matching{PipelineFixture::make_engine(), std::move(*command_consumer),
                         std::move(*event_producer)};
  PublicationStage publication{std::move(*event_consumer)};

  EXPECT_EQ(
      ingress.try_ingest(
          CommandPayload::submit_limit(OrderId{1U}, Side::sell, Price{102}, Quantity{2U}), 1U),
      IngressStatus::progress);
  EXPECT_EQ(ingress.journal_size(), 1U);
  EXPECT_EQ(matching.process_one(), MatchingStatus::progress);
  EXPECT_EQ(
      ingress.try_ingest(CommandPayload::submit_market(OrderId{2U}, Side::buy, Quantity{2U}), 2U),
      IngressStatus::progress);
  EXPECT_EQ(matching.process_one(), MatchingStatus::progress);

  std::array<EngineEvent, 3U> actual{};
  std::size_t count{};
  EXPECT_EQ(publication.drain([&](const EngineEvent& event) noexcept { actual[count++] = event; }),
            3U);
  ASSERT_EQ(count, 3U);
  EXPECT_EQ(actual[0].command_sequence, Sequence{1U});
  EXPECT_EQ(actual[0].event_index, 0U);
  EXPECT_EQ(actual[1].command_sequence, Sequence{2U});
  EXPECT_EQ(actual[1].event_index, 0U);
  EXPECT_EQ(actual[2].command_sequence, Sequence{2U});
  EXPECT_EQ(actual[2].event_index, 1U);
}

TEST(PipelineTest, IngressBackpressureConsumesNeitherSequenceNorJournal) {
  PipelineFixture fixture;
  SpscQueue<SequencedCommand> commands{1U};
  auto journal = MmapJournal::create(fixture.temporary.path() / "commands.journal", 2U);
  ASSERT_TRUE(journal.has_value());
  auto producer = commands.claim_producer();
  auto consumer = commands.claim_consumer();
  ASSERT_TRUE(producer && consumer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};

  const CommandPayload payload =
      CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U});
  EXPECT_EQ(ingress.try_ingest(payload, 1U), IngressStatus::progress);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), IngressStatus::queue_backpressure);
  EXPECT_EQ(ingress.next_sequence(), Sequence{2U});
  EXPECT_EQ(ingress.journal_size(), 1U);
}

TEST(PipelineTest, JournalFullIsDetectedBeforeStamping) {
  PipelineFixture fixture;
  auto journal = MmapJournal::create(fixture.temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  auto producer = fixture.commands.claim_producer();
  ASSERT_TRUE(producer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};
  const CommandPayload payload =
      CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U});
  ASSERT_EQ(ingress.try_ingest(payload, 1U), IngressStatus::progress);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), IngressStatus::journal_full);
  EXPECT_EQ(ingress.next_sequence(), Sequence{2U});
}

TEST(PipelineTest, PersistenceFailurePoisonsIngressAfterStamping) {
  PipelineFixture fixture;
  const auto path = fixture.temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(path, 2U);
  ASSERT_TRUE(journal.has_value());
  auto producer = fixture.commands.claim_producer();
  ASSERT_TRUE(producer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};
  journal_testing::fail_for_journal(
      ingress.journal_identity_for_testing(),
      journal_testing::failure_mask(journal_testing::FailurePoint::append_pre_publish_msync));
  const CommandPayload payload =
      CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U});

  EXPECT_EQ(ingress.try_ingest(payload, 1U), IngressStatus::persistence_failure);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), IngressStatus::recovery_required);
  EXPECT_TRUE(fixture.commands.empty());
}

TEST(PipelineTest, IndeterminatePostCommitFailureRequiresRecoveryWithoutPublishing) {
  PipelineFixture fixture;
  const auto path = fixture.temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(path, 2U);
  ASSERT_TRUE(journal.has_value());
  auto producer = fixture.commands.claim_producer();
  ASSERT_TRUE(producer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};
  journal_testing::fail_for_journal(
      ingress.journal_identity_for_testing(),
      journal_testing::failure_mask(journal_testing::FailurePoint::append_post_publish_msync));

  EXPECT_EQ(
      ingress.try_ingest(CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), 1U),
      IngressStatus::commit_indeterminate);
  EXPECT_EQ(ingress.last_journal_error(), JournalError::commit_indeterminate);
  EXPECT_TRUE(fixture.commands.empty());
  EXPECT_EQ(
      ingress.try_ingest(CommandPayload::submit_market(OrderId{2U}, Side::buy, Quantity{1U}), 2U),
      IngressStatus::recovery_required);
}

TEST(PipelineTest, MatchingConstructionRequiresMaximumEventBatchCapacity) {
  CommandQueue commands{1U};
  EventQueue too_small{3U};
  auto command_consumer = commands.claim_consumer();
  auto event_producer = too_small.claim_producer();
  ASSERT_TRUE(command_consumer && event_producer);
  EXPECT_THROW((MatchingStage{PipelineFixture::make_engine(), std::move(*command_consumer),
                              std::move(*event_producer)}),
               std::invalid_argument);

  CommandQueue exact_commands{1U};
  EventQueue exact_events{4U};
  auto exact_consumer = exact_commands.claim_consumer();
  auto exact_producer = exact_events.claim_producer();
  ASSERT_TRUE(exact_consumer && exact_producer);
  EXPECT_NO_THROW((MatchingStage{PipelineFixture::make_engine(), std::move(*exact_consumer),
                                 std::move(*exact_producer)}));

  CommandQueue empty_commands{1U};
  EventQueue one_event{1U};
  auto empty_consumer = empty_commands.claim_consumer();
  auto one_producer = one_event.claim_producer();
  ASSERT_TRUE(empty_consumer && one_producer);
  EXPECT_NO_THROW((MatchingStage{
      std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 1U}, 0U, Quantity{1U}),
      std::move(*empty_consumer), std::move(*one_producer)}));
}

TEST(PipelineIngressStatusTest, InvalidPayloadPrecedesBackpressureWithoutMutation) {
  PipelineTemporaryDirectory temporary;
  CommandQueue commands{1U};
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 2U);
  ASSERT_TRUE(journal);
  auto producer = commands.claim_producer();
  ASSERT_TRUE(producer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};
  ASSERT_EQ(
      ingress.try_ingest(CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), 1U),
      IngressStatus::progress);
  CommandPayload invalid = CommandPayload::cancel(Handle{1U, 1U});
  invalid.order_id = 9U;

  EXPECT_EQ(ingress.try_ingest(invalid, 2U), IngressStatus::invalid_payload);
  EXPECT_EQ(ingress.next_sequence(), Sequence{2U});
  EXPECT_EQ(ingress.journal_size(), 1U);
}

TEST(PipelineIngressStatusTest, DecreasingTimeAndExhaustionDoNotPersistOrPublish) {
  PipelineTemporaryDirectory temporary;
  CommandQueue commands{1U};
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 3U);
  ASSERT_TRUE(journal);
  auto producer = commands.claim_producer();
  auto consumer = commands.claim_consumer();
  ASSERT_TRUE(producer && consumer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*producer)};
  const CommandPayload payload =
      CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U});
  ASSERT_EQ(ingress.try_ingest(payload, 5U), IngressStatus::progress);
  SequencedCommand popped{};
  ASSERT_TRUE(consumer->try_pop(popped));

  EXPECT_EQ(ingress.try_ingest(payload, 4U), IngressStatus::decreasing_logical_time);
  EXPECT_EQ(ingress.next_sequence(), Sequence{2U});
  EXPECT_EQ(ingress.journal_size(), 1U);
  EXPECT_TRUE(commands.empty());

  CommandQueue terminal_commands{1U};
  auto terminal_journal = MmapJournal::create(temporary.path() / "terminal.journal", 2U);
  ASSERT_TRUE(terminal_journal);
  auto terminal_producer = terminal_commands.claim_producer();
  ASSERT_TRUE(terminal_producer);
  Sequencer exhausted{Sequence{std::numeric_limits<std::uint64_t>::max()}, 0U};
  ASSERT_TRUE(exhausted.stamp(payload, 1U));
  DurableIngressStage terminal{exhausted, std::move(*terminal_journal),
                               std::move(*terminal_producer)};

  EXPECT_EQ(terminal.try_ingest(payload, 2U), IngressStatus::sequence_exhausted);
  EXPECT_EQ(terminal.journal_size(), 0U);
  EXPECT_TRUE(terminal_commands.empty());
}

TEST(PipelineTest, OutputBackpressureLeavesInputAndEngineUntouched) {
  PipelineFixture fixture;
  SpscQueue<EngineEvent> events{4U};
  auto command_producer = fixture.commands.claim_producer();
  auto command_consumer = fixture.commands.claim_consumer();
  auto event_producer = events.claim_producer();
  auto event_consumer = events.claim_consumer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer && event_consumer);
  ASSERT_TRUE(command_producer->try_push(
      {CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), Sequence{1U}, 1U}));
  ASSERT_TRUE(event_producer->try_push({.command_sequence = Sequence{99U}}));
  MatchingStage matching{PipelineFixture::make_engine(), std::move(*command_consumer),
                         std::move(*event_producer)};

  EXPECT_EQ(matching.process_one(), MatchingStatus::output_backpressure);
  EXPECT_EQ(matching.engine().next_sequence(), Sequence{1U});
  EXPECT_FALSE(fixture.commands.empty());
}

TEST(PipelineMatchingStatusTest, InvalidCommandAndDecreasingTimePoisonWithoutDroppingInput) {
  const auto run = [](SequencedCommand command, std::uint64_t initial_time,
                      MatchingStatus expected) {
    CommandQueue commands{1U};
    EventQueue events{2U};
    auto producer = commands.claim_producer();
    auto consumer = commands.claim_consumer();
    auto event_producer = events.claim_producer();
    ASSERT_TRUE(producer && consumer && event_producer);
    ASSERT_TRUE(producer->try_push(command));
    MatchingStage matching{std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 1U}, 1U,
                                                             Quantity{10U}, Sequence{1U},
                                                             initial_time),
                           std::move(*consumer), std::move(*event_producer)};

    EXPECT_EQ(matching.process_one(), expected);
    EXPECT_EQ(matching.process_one(), MatchingStatus::poisoned);
    EXPECT_EQ(matching.engine().next_sequence(), Sequence{1U});
    EXPECT_FALSE(commands.empty());
    EXPECT_TRUE(events.empty());
  };

  SequencedCommand invalid{CommandPayload::cancel(Handle{1U, 1U}), Sequence{1U}, 10U};
  invalid.payload.order_id = 7U;
  run(invalid, 0U, MatchingStatus::invalid_command);
  run({CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), Sequence{1U}, 9U}, 10U,
      MatchingStatus::decreasing_logical_time);
}

TEST(PipelineMatchingStatusTest, SequenceExhaustionIsReportedWithoutDroppingInput) {
  CommandQueue commands{2U};
  EventQueue events{2U};
  auto producer = commands.claim_producer();
  auto consumer = commands.claim_consumer();
  auto event_producer = events.claim_producer();
  ASSERT_TRUE(producer && consumer && event_producer);
  const SequencedCommand final{CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}),
                               Sequence{std::numeric_limits<std::uint64_t>::max()}, 1U};
  ASSERT_TRUE(producer->try_push(final));
  ASSERT_TRUE(producer->try_push(final));
  MatchingStage matching{
      std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 1U}, 0U, Quantity{1U},
                                        Sequence{std::numeric_limits<std::uint64_t>::max()}, 0U),
      std::move(*consumer), std::move(*event_producer)};

  ASSERT_EQ(matching.process_one(), MatchingStatus::progress);
  EXPECT_EQ(matching.process_one(), MatchingStatus::sequence_exhausted);
  EXPECT_EQ(matching.process_one(), MatchingStatus::poisoned);
  EXPECT_FALSE(commands.empty());
}

TEST(PipelineTest, ApplyMismatchPoisonsRatherThanSkipping) {
  PipelineFixture fixture;
  auto command_producer = fixture.commands.claim_producer();
  auto command_consumer = fixture.commands.claim_consumer();
  auto event_producer = fixture.events.claim_producer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer);
  ASSERT_TRUE(command_producer->try_push(
      {CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), Sequence{2U}, 1U}));
  MatchingStage matching{PipelineFixture::make_engine(), std::move(*command_consumer),
                         std::move(*event_producer)};

  EXPECT_EQ(matching.process_one(), MatchingStatus::invalid_sequence);
  EXPECT_EQ(matching.process_one(), MatchingStatus::poisoned);
  EXPECT_EQ(matching.engine().next_sequence(), Sequence{1U});
  EXPECT_FALSE(fixture.commands.empty());
  EXPECT_TRUE(fixture.events.empty());
}

TEST(PipelineTest, SnapshotOccursAtACommandBoundary) {
  PipelineFixture fixture;
  auto command_producer = fixture.commands.claim_producer();
  auto command_consumer = fixture.commands.claim_consumer();
  auto event_producer = fixture.events.claim_producer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer);
  ASSERT_TRUE(command_producer->try_push(
      {CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), Sequence{1U}, 7U}));
  MatchingStage matching{PipelineFixture::make_engine(), std::move(*command_consumer),
                         std::move(*event_producer)};
  ASSERT_EQ(matching.process_one(), MatchingStatus::progress);

  const auto path = fixture.temporary.path() / "engine.snapshot";
  EXPECT_EQ(matching.save_snapshot(path), SnapshotError::none);
  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->point, (SnapshotPoint{Sequence{1U}, 7U}));
  EXPECT_EQ(loaded->engine->next_sequence(), Sequence{2U});
}

TEST(PipelineThreadedTest, MixedValidWorkloadMatchesReferenceExactly) {
  constexpr std::size_t cycles = 32U;
  constexpr std::size_t max_orders = 16U;
  Sequencer reference_sequencer;
  SequencedEngine reference{PriceDomain{Price{100}, 5U}, max_orders, Quantity{100U}};
  std::array<EngineEvent, max_orders + 1U> reference_scratch{};
  std::vector<CommandPayload> payloads;
  std::vector<EngineEvent> expected;
  std::vector<Handle> retired_handles;
  payloads.reserve(cycles * 16U);
  expected.reserve(cycles * 24U);
  retired_handles.reserve(cycles * 6U);
  bool reference_ok = true;
  const auto apply_reference = [&](const CommandPayload& payload) {
    payloads.push_back(payload);
    const auto command = reference_sequencer.stamp(payload, payloads.size());
    if (!command) {
      reference_ok = false;
      return Handle{};
    }
    const ApplyResult result = reference.apply(*command, reference_scratch);
    if (result.status != ApplyStatus::applied || result.event_count == 0U) {
      reference_ok = false;
      return Handle{};
    }
    expected.insert(expected.end(), reference_scratch.begin(),
                    reference_scratch.begin() + static_cast<std::ptrdiff_t>(result.event_count));
    return reference_scratch[0].handle;
  };

  for (std::size_t cycle = 0U; cycle < cycles; ++cycle) {
    const std::uint64_t id = cycle * 100U;
    const Handle first_ask = apply_reference(
        CommandPayload::submit_limit(OrderId{id + 1U}, Side::sell, Price{102}, Quantity{5U}));
    const Handle second_ask = apply_reference(
        CommandPayload::submit_limit(OrderId{id + 2U}, Side::sell, Price{103}, Quantity{4U}));
    static_cast<void>(apply_reference(
        CommandPayload::submit_limit(OrderId{id + 3U}, Side::buy, Price{103}, Quantity{7U})));
    static_cast<void>(apply_reference(CommandPayload::amend_quantity(second_ask, Quantity{1U})));
    const Handle replaced_ask =
        apply_reference(CommandPayload::replace(second_ask, Price{104}, Quantity{3U}));
    const Handle low_ask = apply_reference(
        CommandPayload::submit_limit(OrderId{id + 4U}, Side::sell, Price{101}, Quantity{2U}));
    static_cast<void>(apply_reference(CommandPayload::submit_limit(
        OrderId{id + 5U}, Side::buy, Price{101}, Quantity{3U}, TimeInForce::fok)));
    static_cast<void>(
        apply_reference(CommandPayload::submit_market(OrderId{id + 6U}, Side::buy, Quantity{3U})));
    static_cast<void>(apply_reference(CommandPayload::cancel(replaced_ask)));
    static_cast<void>(apply_reference(CommandPayload::cancel(replaced_ask)));
    static_cast<void>(
        apply_reference(CommandPayload::submit_market(OrderId{id + 7U}, Side::sell, Quantity{1U})));
    const Handle bid = apply_reference(
        CommandPayload::submit_limit(OrderId{id + 8U}, Side::buy, Price{100}, Quantity{4U}));
    static_cast<void>(apply_reference(CommandPayload::amend_quantity(bid, Quantity{2U})));
    const Handle replaced_bid =
        apply_reference(CommandPayload::replace(bid, Price{101}, Quantity{3U}));
    static_cast<void>(apply_reference(CommandPayload::cancel(replaced_bid)));
    static_cast<void>(apply_reference(
        CommandPayload::submit_limit(OrderId{id + 9U}, Side::buy, Price{100}, Quantity{0U})));
    retired_handles.insert(retired_handles.end(),
                           {first_ask, second_ask, replaced_ask, low_ask, bid, replaced_bid});
  }
  ASSERT_TRUE(reference_ok);
  ASSERT_GT(expected.size(), payloads.size());
  ASSERT_EQ(reference.order_book().best_bid(), std::nullopt);
  ASSERT_EQ(reference.order_book().best_ask(), std::nullopt);

  PipelineTemporaryDirectory temporary;
  CommandQueue commands{32U};
  EventQueue events{32U};
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", payloads.size());
  ASSERT_TRUE(journal);
  auto command_producer = commands.claim_producer();
  auto command_consumer = commands.claim_consumer();
  auto event_producer = events.claim_producer();
  auto event_consumer = events.claim_consumer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer && event_consumer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*command_producer)};
  MatchingStage matching{
      std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 5U}, max_orders, Quantity{100U}),
      std::move(*command_consumer), std::move(*event_producer)};
  PublicationStage publication{std::move(*event_consumer)};

  std::stop_source stop;
  std::atomic<bool> ingress_done{};
  std::atomic<bool> matching_done{};
  std::atomic<bool> completed{};
  std::atomic<bool> failed{};
  const auto fail = [&] {
    failed.store(true, std::memory_order_release);
    stop.request_stop();
  };
  std::vector<EngineEvent> actual;
  actual.reserve(expected.size());
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
  std::jthread ingress_thread{[&] {
    std::size_t index{};
    while (index < payloads.size() && !stop.stop_requested()) {
      const IngressStatus status = ingress.try_ingest(payloads[index], index + 1U);
      if (status == IngressStatus::progress) {
        ++index;
      } else if (status != IngressStatus::queue_backpressure) {
        fail();
      } else {
        std::this_thread::yield();
      }
    }
    if (index != payloads.size() && !stop.stop_requested()) {
      fail();
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
      } else if (status != MatchingStatus::progress) {
        std::this_thread::yield();
      }
    }
    if (!commands.empty() && !stop.stop_requested()) {
      fail();
    }
    matching_done.store(true, std::memory_order_release);
  }};
  std::jthread publication_thread{[&] {
    while (!matching_done.load(std::memory_order_acquire) || !events.empty()) {
      EngineEvent event{};
      if (publication.try_pop(event) == PublicationStatus::progress) {
        actual.push_back(event);
      } else if (stop.stop_requested() && matching_done.load(std::memory_order_acquire)) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
    if (!events.empty()) {
      fail();
    }
  }};
  ingress_thread.join();
  matching_thread.join();
  publication_thread.join();
  completed.store(true, std::memory_order_release);
  watchdog.request_stop();
  watchdog.join();

  ASSERT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_EQ(actual, expected);
  ReplayFingerprint actual_fingerprint;
  ReplayFingerprint expected_fingerprint;
  for (const EngineEvent& event : actual) {
    ASSERT_EQ(actual_fingerprint.add(event), EventCodecError::none);
  }
  for (const EngineEvent& event : expected) {
    ASSERT_EQ(expected_fingerprint.add(event), EventCodecError::none);
  }
  EXPECT_EQ(actual_fingerprint.event_count(), expected_fingerprint.event_count());
  EXPECT_EQ(actual_fingerprint.crc32c(), expected_fingerprint.crc32c());
  EXPECT_EQ(matching.engine().next_sequence(), reference.next_sequence());
  EXPECT_EQ(matching.engine().last_logical_time(), reference.last_logical_time());
  EXPECT_EQ(matching.engine().order_book().check_invariants(),
            reference.order_book().check_invariants());
  EXPECT_EQ(matching.engine().order_book().best_bid(), reference.order_book().best_bid());
  EXPECT_EQ(matching.engine().order_book().best_ask(), reference.order_book().best_ask());
  for (std::int64_t ticks = 100; ticks <= 104; ++ticks) {
    EXPECT_EQ(matching.engine().order_book().level_info(Side::buy, Price{ticks}),
              reference.order_book().level_info(Side::buy, Price{ticks}));
    EXPECT_EQ(matching.engine().order_book().level_info(Side::sell, Price{ticks}),
              reference.order_book().level_info(Side::sell, Price{ticks}));
  }
  for (const Handle handle : retired_handles) {
    EXPECT_EQ(matching.engine().order_book().order_info(handle),
              reference.order_book().order_info(handle));
  }
}

TEST(PipelineThreadedTest, InjectedMatcherFailureStopsPeersAndDrainsPublishedEvents) {
  CommandQueue commands{1U};
  EventQueue events{2U};
  auto command_producer = commands.claim_producer();
  auto command_consumer = commands.claim_consumer();
  auto event_producer = events.claim_producer();
  auto event_consumer = events.claim_consumer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer && event_consumer);
  ASSERT_TRUE(event_producer->try_push({.command_sequence = Sequence{99U}}));
  MatchingStage matching{
      std::make_unique<SequencedEngine>(PriceDomain{Price{100}, 1U}, 1U, Quantity{10U}),
      std::move(*command_consumer), std::move(*event_producer)};
  PublicationStage publication{std::move(*event_consumer)};
  std::stop_source stop;
  std::atomic<bool> ingress_stopped{};
  std::atomic<bool> matching_done{};
  std::atomic<bool> completed{};
  std::atomic<bool> failed{};
  std::atomic<std::size_t> published{};
  std::jthread watchdog{[&](const std::stop_token& token) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!token.stop_requested() && !completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!completed.load(std::memory_order_acquire) && !token.stop_requested()) {
      failed.store(true, std::memory_order_release);
      stop.request_stop();
    }
  }};
  std::jthread ingress_thread{
      [endpoint = std::move(*command_producer), &stop, &failed, &ingress_stopped]() mutable {
        if (!endpoint.try_push({CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}),
                                Sequence{2U}, 1U})) {
          failed.store(true, std::memory_order_release);
          stop.request_stop();
        }
        while (!stop.stop_requested()) {
          std::this_thread::yield();
        }
        ingress_stopped.store(true, std::memory_order_release);
      }};
  std::jthread matching_thread{[&] {
    while (!stop.stop_requested()) {
      const MatchingStatus status = matching.process_one();
      if (status == MatchingStatus::invalid_sequence) {
        stop.request_stop();
      } else if (status != MatchingStatus::input_empty) {
        failed.store(true, std::memory_order_release);
        stop.request_stop();
      }
    }
    matching_done.store(true, std::memory_order_release);
  }};
  std::jthread publication_thread{[&] {
    while (!matching_done.load(std::memory_order_acquire) || !events.empty()) {
      EngineEvent event{};
      if (publication.try_pop(event) == PublicationStatus::progress) {
        published.fetch_add(1U, std::memory_order_relaxed);
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

  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_TRUE(ingress_stopped.load(std::memory_order_acquire));
  EXPECT_EQ(published.load(std::memory_order_relaxed), 1U);
  EXPECT_TRUE(events.empty());
  EXPECT_FALSE(commands.empty());
}

} // namespace
} // namespace matching_engine
