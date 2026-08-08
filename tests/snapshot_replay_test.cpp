#include "matching_engine/replay.hpp"
#include "matching_engine/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>

namespace matching_engine {
namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("matching-engine-snapshot-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
             "-" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

TEST(EngineEventCodecTest, HasCanonicalLittleEndianGoldenVector) {
  const EngineEvent event{.command_sequence = Sequence{0x0102030405060708ULL},
                          .order_id = OrderId{0x1112131415161718ULL},
                          .secondary_order_id = OrderId{0x2122232425262728ULL},
                          .price = Price{-2},
                          .quantity = Quantity{0x3132333435363738ULL},
                          .secondary_quantity = Quantity{0x4142434445464748ULL},
                          .handle = Handle{0x51525354U, 0x61626364U},
                          .event_index = 0x71727374U,
                          .type = EngineEventType::amend_result,
                          .reason = 0x81U};
  std::array<std::byte, kEncodedEngineEventSize> bytes{};
  ASSERT_EQ(encode_engine_event(event, bytes), EventCodecError::none);
  EXPECT_EQ(bytes[0], std::byte{0x08});
  EXPECT_EQ(bytes[7], std::byte{0x01});
  EXPECT_EQ(bytes[24], std::byte{0xfe});
  EXPECT_EQ(bytes[31], std::byte{0xff});
  EXPECT_EQ(bytes[56], std::byte{0x74});
  EXPECT_EQ(bytes[60], std::byte{0x03});
  EXPECT_EQ(bytes[61], std::byte{0x81});
  EXPECT_EQ(bytes[62], std::byte{0x00});
  EXPECT_EQ(bytes[63], std::byte{0x00});
  EXPECT_EQ(decode_engine_event(bytes), event);
}

TEST(ReplayFingerprintTest, StreamsCanonicalEventBytes) {
  const EngineEvent event{.command_sequence = Sequence{1U},
                          .order_id = OrderId{2U},
                          .price = Price{3},
                          .quantity = Quantity{4U},
                          .handle = Handle{5U, 6U},
                          .type = EngineEventType::submit_result};
  ReplayFingerprint fingerprint;
  ASSERT_EQ(fingerprint.add(event), EventCodecError::none);
  EXPECT_EQ(fingerprint.event_count(), 1U);
  EXPECT_EQ(fingerprint.byte_count(), kEncodedEngineEventSize);
  EXPECT_EQ(fingerprint.crc32c(), 0x48a49a71U);
}

TEST(SnapshotCodecTest, EmptyStateHasStableSizeAndRoundTrips) {
  SequencedEngine engine{PriceDomain{Price{-4}, 9U}, 3U, Quantity{100U}};
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded->size(), kSnapshotHeaderSize + 3U * kSnapshotSlotSize);

  auto restored = decode_snapshot(*encoded);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->point, (SnapshotPoint{Sequence{0U}, 0U}));
  EXPECT_EQ(restored->engine->order_book().check_invariants().violation, InvariantViolation::none);
  EXPECT_EQ(restored->engine->order_book().best_bid(), std::nullopt);
}

TEST(SnapshotCodecTest, PreservesGenerationAndFifoState) {
  SequencedEngine engine{PriceDomain{Price{100}, 5U}, 3U, Quantity{100U}};
  std::array<EngineEvent, 4U> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{101}, Quantity{5U}),
                  Sequence{1U}, 10U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle canceled = events[0].handle;
  ASSERT_EQ(engine.apply({CommandPayload::cancel(canceled), Sequence{2U}, 11U}, events).status,
            ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{103}, Quantity{7U}),
                  Sequence{3U}, 12U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle live = events[0].handle;

  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{3U}, 12U});
  ASSERT_TRUE(encoded.has_value());
  auto restored = decode_snapshot(*encoded);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->engine->order_book().order_info(live),
            (OrderInfo{OrderId{2U}, Side::sell, Price{103}, Quantity{7U}}));
  EXPECT_FALSE(restored->engine->order_book().order_info(canceled).has_value());
  EXPECT_EQ(restored->engine->order_book().best_ask(), Price{103});

  ASSERT_EQ(
      restored->engine->apply({CommandPayload::cancel(live), Sequence{4U}, 13U}, events).status,
      ApplyStatus::applied);
  ASSERT_EQ(
      restored->engine
          ->apply({CommandPayload::submit_limit(OrderId{3U}, Side::buy, Price{100}, Quantity{1U}),
                   Sequence{5U}, 14U},
                  events)
          .status,
      ApplyStatus::applied);
  EXPECT_EQ(events[0].handle.index, live.index);
  EXPECT_GT(events[0].handle.generation, live.generation);
}

TEST(SnapshotCodecTest, RejectsCorruptionAndNoncanonicalDeadPayload) {
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 1U, Quantity{1U}};
  auto bytes = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(bytes.has_value());
  (*bytes)[8] = std::byte{2U};
  EXPECT_EQ(decode_snapshot(*bytes).error(), SnapshotError::unsupported_version);

  bytes = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(bytes.has_value());
  (*bytes)[kSnapshotHeaderSize + 12U] = std::byte{1U};
  rewrite_snapshot_crc_for_testing(*bytes);
  EXPECT_EQ(decode_snapshot(*bytes).error(), SnapshotError::noncanonical_slot);
}

TEST(SnapshotPersistenceTest, AtomicallyOverwritesAndLoadsMode0600File) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine first{PriceDomain{Price{10}, 2U}, 1U, Quantity{5U}};
  ASSERT_EQ(save_snapshot_atomic(path, first, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  SequencedEngine second{PriceDomain{Price{20}, 3U}, 2U, Quantity{9U}};
  std::array<EngineEvent, 3U> events{};
  ASSERT_EQ(
      second
          .apply({CommandPayload::submit_limit(OrderId{7U}, Side::buy, Price{21}, Quantity{4U}),
                  Sequence{1U}, 5U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(save_snapshot_atomic(path, second, SnapshotPoint{Sequence{1U}, 5U}),
            SnapshotError::none);

  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->point, (SnapshotPoint{Sequence{1U}, 5U}));
  EXPECT_EQ(loaded->engine->order_book().best_bid(), Price{21});
  EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

TEST(SnapshotPersistenceTest, RejectsSymlinkAndWrongMode) {
  TemporaryDirectory temporary;
  const auto real = temporary.path() / "real.snapshot";
  const auto link = temporary.path() / "link.snapshot";
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  ASSERT_EQ(save_snapshot_atomic(real, engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  std::filesystem::create_symlink(real.filename(), link);
  EXPECT_EQ(load_snapshot(link).error(), SnapshotError::symlink);
  std::filesystem::permissions(real, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add);
  EXPECT_EQ(load_snapshot(real).error(), SnapshotError::permission_denied);
}

TEST(SnapshotPersistenceTest, PreRenameFailureKeepsOldSnapshotAndCleansTemp) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine old_engine{PriceDomain{Price{10}, 1U}, 0U, Quantity{1U}};
  ASSERT_EQ(save_snapshot_atomic(path, old_engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  SequencedEngine replacement{PriceDomain{Price{20}, 1U}, 0U, Quantity{1U}};
  snapshot_testing::fail_for_path(
      path, snapshot_testing::failure_mask(snapshot_testing::FailurePoint::rename));

  EXPECT_EQ(save_snapshot_atomic(path, replacement, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::io_error);
  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->engine->order_book().level_info(Side::buy, Price{10})->order_count, 0U);
  std::size_t entries{};
  for (const auto& ignored : std::filesystem::directory_iterator(temporary.path())) {
    static_cast<void>(ignored);
    ++entries;
  }
  EXPECT_EQ(entries, 1U);
}

TEST(SnapshotPersistenceTest, PostRenameDirectorySyncFailureIsIndeterminate) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine engine{PriceDomain{Price{10}, 1U}, 0U, Quantity{1U}};
  snapshot_testing::fail_for_path(
      path, snapshot_testing::failure_mask(snapshot_testing::FailurePoint::parent_fsync));

  EXPECT_EQ(save_snapshot_atomic(path, engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::commit_indeterminate);
  EXPECT_TRUE(load_snapshot(path).has_value());
}

TEST(ReplayTest, SnapshotBoundarySkipsPrefixAndReplaysExactSuffix) {
  TemporaryDirectory temporary;
  const auto journal_path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(journal_path, 4U);
  ASSERT_TRUE(journal.has_value());
  const std::array commands{
      SequencedCommand{
          CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{101}, Quantity{5U}),
          Sequence{1U}, 10U},
      SequencedCommand{
          CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{103}, Quantity{4U}),
          Sequence{2U}, 11U},
      SequencedCommand{CommandPayload::submit_market(OrderId{3U}, Side::buy, Quantity{2U}),
                       Sequence{3U}, 12U},
      SequencedCommand{CommandPayload::amend_quantity(Handle{0U, 1U}, Quantity{3U}), Sequence{4U},
                       13U},
  };
  SequencedEngine uninterrupted{PriceDomain{Price{100}, 5U}, 4U, Quantity{10U}};
  std::array<EngineEvent, 5U> events{};
  for (std::size_t index = 0; index < 2U; ++index) {
    ASSERT_EQ(journal->append(commands[index]), JournalError::none);
    ASSERT_EQ(uninterrupted.apply(commands[index], events).status, ApplyStatus::applied);
  }
  const auto snapshot = encode_snapshot(uninterrupted, SnapshotPoint{Sequence{2U}, 11U});
  ASSERT_TRUE(snapshot.has_value());
  ReplayFingerprint expected;
  for (std::size_t index = 2U; index < commands.size(); ++index) {
    ASSERT_EQ(journal->append(commands[index]), JournalError::none);
    const ApplyResult applied = uninterrupted.apply(commands[index], events);
    ASSERT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t event_index = 0; event_index < applied.event_count; ++event_index) {
      ASSERT_EQ(expected.add(events[event_index]), EventCodecError::none);
    }
  }
  auto restored = decode_snapshot(*snapshot);
  ASSERT_TRUE(restored.has_value());
  auto replayed = replay_journal(*journal, *restored->engine, restored->point.sequence,
                                 restored->point.logical_time, events);
  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 2U);
  EXPECT_EQ(replayed->fingerprint.event_count(), expected.event_count());
  EXPECT_EQ(replayed->fingerprint.crc32c(), expected.crc32c());
  EXPECT_EQ(restored->engine->order_book().best_bid(), uninterrupted.order_book().best_bid());
  EXPECT_EQ(restored->engine->order_book().best_ask(), uninterrupted.order_book().best_ask());
}

} // namespace
} // namespace matching_engine
