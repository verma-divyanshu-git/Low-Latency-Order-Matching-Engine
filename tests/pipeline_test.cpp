#include "matching_engine/pipeline.hpp"
#include "matching_engine/replay.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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
      PipelineStatus::progress);
  EXPECT_EQ(ingress.journal_size(), 1U);
  EXPECT_EQ(matching.process_one(), PipelineStatus::progress);
  EXPECT_EQ(
      ingress.try_ingest(CommandPayload::submit_market(OrderId{2U}, Side::buy, Quantity{2U}), 2U),
      PipelineStatus::progress);
  EXPECT_EQ(matching.process_one(), PipelineStatus::progress);

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
  EXPECT_EQ(ingress.try_ingest(payload, 1U), PipelineStatus::progress);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), PipelineStatus::ingress_backpressure);
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
  ASSERT_EQ(ingress.try_ingest(payload, 1U), PipelineStatus::progress);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), PipelineStatus::journal_full);
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

  EXPECT_EQ(ingress.try_ingest(payload, 1U), PipelineStatus::persistence_required);
  EXPECT_EQ(ingress.try_ingest(payload, 2U), PipelineStatus::stopped_poisoned);
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
      PipelineStatus::persistence_required);
  EXPECT_EQ(ingress.last_journal_error(), JournalError::commit_indeterminate);
  EXPECT_TRUE(fixture.commands.empty());
}

TEST(PipelineTest, OutputBackpressureLeavesInputAndEngineUntouched) {
  PipelineFixture fixture;
  SpscQueue<EngineEvent> events{1U};
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

  EXPECT_EQ(matching.process_one(), PipelineStatus::output_backpressure);
  EXPECT_EQ(matching.engine().next_sequence(), Sequence{1U});
  EXPECT_FALSE(fixture.commands.empty());
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

  EXPECT_EQ(matching.process_one(), PipelineStatus::stopped_poisoned);
  EXPECT_EQ(matching.process_one(), PipelineStatus::stopped_poisoned);
  EXPECT_FALSE(fixture.commands.empty());
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
  ASSERT_EQ(matching.process_one(), PipelineStatus::progress);

  const auto path = fixture.temporary.path() / "engine.snapshot";
  EXPECT_EQ(matching.save_snapshot(path), SnapshotError::none);
  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->point, (SnapshotPoint{Sequence{1U}, 7U}));
  EXPECT_EQ(loaded->engine->next_sequence(), Sequence{2U});
}

TEST(PipelineThreadedTest, ThreeExternallyScheduledStagesMatchReferenceExactly) {
  constexpr std::size_t command_count = 256U;
  PipelineTemporaryDirectory temporary;
  CommandQueue commands{32U};
  EventQueue events{32U};
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", command_count);
  ASSERT_TRUE(journal.has_value());
  auto command_producer = commands.claim_producer();
  auto command_consumer = commands.claim_consumer();
  auto event_producer = events.claim_producer();
  auto event_consumer = events.claim_consumer();
  ASSERT_TRUE(command_producer && command_consumer && event_producer && event_consumer);
  DurableIngressStage ingress{Sequencer{}, std::move(*journal), std::move(*command_producer)};
  MatchingStage matching{PipelineFixture::make_engine(), std::move(*command_consumer),
                         std::move(*event_producer)};
  PublicationStage publication{std::move(*event_consumer)};

  std::array<CommandPayload, command_count> payloads{};
  for (std::size_t index = 0U; index < command_count; ++index) {
    switch (index % 5U) {
    case 0U:
      payloads[index] = CommandPayload::submit_limit(
          OrderId{index + 1U}, index % 2U == 0U ? Side::buy : Side::sell,
          Price{100 + static_cast<std::int64_t>(index % 5U)}, Quantity{1U});
      break;
    case 1U:
      payloads[index] = CommandPayload::submit_market(OrderId{index + 1U}, Side::buy, Quantity{1U});
      break;
    case 2U:
      payloads[index] = CommandPayload::cancel(Handle{kInvalidIndex, 1U});
      break;
    case 3U:
      payloads[index] = CommandPayload::amend_quantity(Handle{kInvalidIndex, 1U}, Quantity{1U});
      break;
    default:
      payloads[index] =
          CommandPayload::replace(Handle{kInvalidIndex, 1U}, Price{101}, Quantity{1U});
      break;
    }
  }

  Sequencer reference_sequencer;
  SequencedEngine reference{PriceDomain{Price{100}, 5U}, 3U, Quantity{100U}};
  std::array<EngineEvent, 4U> reference_scratch{};
  std::vector<EngineEvent> expected;
  expected.reserve(command_count * 2U);
  for (std::size_t index = 0U; index < command_count; ++index) {
    const auto command = reference_sequencer.stamp(payloads[index], index + 1U);
    ASSERT_TRUE(command.has_value());
    const ApplyResult result = reference.apply(*command, reference_scratch);
    ASSERT_EQ(result.status, ApplyStatus::applied);
    expected.insert(expected.end(), reference_scratch.begin(),
                    reference_scratch.begin() + static_cast<std::ptrdiff_t>(result.event_count));
  }

  std::atomic<bool> ingress_done{};
  std::atomic<bool> matching_done{};
  std::atomic<bool> failed{};
  std::vector<EngineEvent> actual;
  actual.reserve(expected.size());
  std::jthread ingress_thread{[&] {
    std::size_t index{};
    std::uint64_t attempts{};
    while (index < command_count && attempts++ < 10'000'000U) {
      const PipelineStatus status = ingress.try_ingest(payloads[index], index + 1U);
      if (status == PipelineStatus::progress) {
        ++index;
      } else if (status != PipelineStatus::ingress_backpressure) {
        failed.store(true, std::memory_order_release);
        break;
      }
      std::this_thread::yield();
    }
    if (index != command_count) {
      failed.store(true, std::memory_order_release);
    }
    ingress_done.store(true, std::memory_order_release);
  }};
  std::jthread matching_thread{[&] {
    std::uint64_t attempts{};
    while ((!ingress_done.load(std::memory_order_acquire) || !commands.empty()) &&
           attempts++ < 10'000'000U) {
      const PipelineStatus status = matching.process_one();
      if (status != PipelineStatus::progress && status != PipelineStatus::input_empty &&
          status != PipelineStatus::output_backpressure) {
        failed.store(true, std::memory_order_release);
        break;
      }
      std::this_thread::yield();
    }
    if (!commands.empty()) {
      failed.store(true, std::memory_order_release);
    }
    matching_done.store(true, std::memory_order_release);
  }};
  std::jthread publication_thread{[&] {
    std::uint64_t attempts{};
    while ((!matching_done.load(std::memory_order_acquire) || !events.empty()) &&
           attempts++ < 10'000'000U) {
      EngineEvent event{};
      if (publication.try_pop(event) == PipelineStatus::progress) {
        actual.push_back(event);
      } else {
        std::this_thread::yield();
      }
    }
    if (!events.empty()) {
      failed.store(true, std::memory_order_release);
    }
  }};
  ingress_thread.join();
  matching_thread.join();
  publication_thread.join();

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
  EXPECT_EQ(matching.engine().order_book().check_invariants(),
            reference.order_book().check_invariants());
  EXPECT_EQ(matching.engine().order_book().best_bid(), reference.order_book().best_bid());
  EXPECT_EQ(matching.engine().order_book().best_ask(), reference.order_book().best_ask());
}

} // namespace
} // namespace matching_engine
