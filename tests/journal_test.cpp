#include "matching_engine/journal.hpp"
#include "matching_engine/sequenced_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace matching_engine {
namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("matching-engine-journal-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
             "-" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void overwrite_byte(const std::filesystem::path& path, std::uint64_t offset, std::byte value) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(descriptor, 0);
  const auto raw = static_cast<unsigned char>(value);
  ASSERT_EQ(::pwrite(descriptor, &raw, 1U, static_cast<off_t>(offset)), 1);
  ASSERT_EQ(::fsync(descriptor), 0);
  ASSERT_EQ(::close(descriptor), 0);
}

void overwrite_record_byte_and_recompute_crc(const std::filesystem::path& path,
                                             std::size_t record_offset, std::byte value) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(descriptor, 0);
  std::array<std::byte, kJournalRecordSize> record{};
  ASSERT_EQ(
      ::pread(descriptor, record.data(), record.size(), static_cast<off_t>(kJournalHeaderSize)),
      static_cast<ssize_t>(record.size()));
  record[record_offset] = value;
  const std::uint32_t crc = crc32c(std::span<const std::byte>{record}.subspan(
      static_cast<std::size_t>(kRecordSequenceOffset),
      static_cast<std::size_t>(kRecordCrcOffset - kRecordSequenceOffset)));
  for (std::size_t index = 0U; index < 4U; ++index) {
    record[static_cast<std::size_t>(kRecordCrcOffset) + index] =
        static_cast<std::byte>((crc >> (index * 8U)) & 0xffU);
  }
  ASSERT_EQ(
      ::pwrite(descriptor, record.data(), record.size(), static_cast<off_t>(kJournalHeaderSize)),
      static_cast<ssize_t>(record.size()));
  ASSERT_EQ(::fsync(descriptor), 0);
  ASSERT_EQ(::close(descriptor), 0);
}

SequencedCommand command(std::uint64_t sequence, std::uint64_t logical_time) {
  return {.payload = CommandPayload::submit_limit(OrderId{sequence}, Side::buy, Price{101},
                                                  Quantity{sequence}, TimeInForce::gtc),
          .sequence = Sequence{sequence},
          .logical_time = logical_time};
}

TEST(Crc32cTest, MatchesPublishedCastagnoliVector) {
  constexpr std::array bytes{std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                             std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
                             std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
  EXPECT_EQ(crc32c(bytes), 0xe3069283U);
}

TEST(JournalTest, CreateAppendCloseReopenAndRead) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 3U);
  ASSERT_TRUE(created.has_value()) << journal_error_message(created.error());
  EXPECT_EQ(created->capacity(), 3U);
  EXPECT_EQ(created->size(), 0U);
  EXPECT_EQ(created->append(command(1, 10)), JournalError::none);
  EXPECT_EQ(created->append(command(2, 10)), JournalError::none);
  EXPECT_EQ(created->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value()) << journal_error_message(reopened.error());
  EXPECT_EQ(reopened->size(), 2U);
  ASSERT_TRUE(reopened->read(0U).has_value());
  EXPECT_EQ(*reopened->read(0U), command(1, 10));
  EXPECT_EQ(*reopened->read(1U), command(2, 10));
  EXPECT_EQ(reopened->read(2U).error(), JournalError::out_of_range);
  EXPECT_EQ(reopened->append(command(3, 11)), JournalError::none);
  EXPECT_EQ(reopened->append(command(4, 12)), JournalError::full);
}

TEST(JournalTest, RejectsInvalidCapacityAndExistingPathWithoutMutation) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  EXPECT_EQ(MmapJournal::create(path, 0U).error(), JournalError::invalid_capacity);
  EXPECT_EQ(MmapJournal::create(path, kMaximumJournalCapacity + 1U).error(),
            JournalError::invalid_capacity);
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(MmapJournal::create(path, 1U).error(), JournalError::already_exists);
}

TEST(JournalTest, RejectsSymlinkAndNonRegularFile) {
  TemporaryDirectory temporary;
  const auto target = temporary.path() / "target";
  {
    std::ofstream output(target);
    output << "not a journal";
  }
  const auto link = temporary.path() / "link";
  ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
  EXPECT_EQ(MmapJournal::open(link).error(), JournalError::symlink);
  EXPECT_EQ(MmapJournal::open(temporary.path()).error(), JournalError::not_regular_file);
}

TEST(JournalTest, RejectsBadPermissionsWhereEnforced) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->close(), JournalError::none);
  ASSERT_EQ(::chmod(path.c_str(), 0000), 0);
  auto opened = MmapJournal::open(path);
  if (::geteuid() != 0) {
    EXPECT_EQ(opened.error(), JournalError::permission_denied);
  }
}

TEST(JournalTest, RejectsHeaderCorruptionReservedBytesAndExactSizeMismatch) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderReservedOffset, std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::invalid_header);
  overwrite_byte(path, kJournalHeaderReservedOffset, std::byte{0});
  ASSERT_EQ(::truncate(path.c_str(), static_cast<off_t>(kJournalHeaderSize)), 0);
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::file_size_mismatch);
}

TEST(JournalTest, ReportsCommittedPayloadBitFlipAsCorruption) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderSize + kRecordPayloadOffset + 4U, std::byte{0xff});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::corrupt_record);
}

TEST(JournalTest, RejectsCommittedNoncanonicalPayloadEvenWithValidCrc) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_record_byte_and_recompute_crc(path, static_cast<std::size_t>(kRecordPayloadOffset) + 3U,
                                          std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::corrupt_record);
}

TEST(JournalTest, StopsAtTornCommitMarkerButNeverSkipsCommittedCorruption) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 2)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize, std::byte{0});
  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->size(), 1U);
}

TEST(JournalTest, RejectsCommittedBadSequenceLogicalTimeAndReservedBytes) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 5)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 6)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordSequenceOffset,
                 std::byte{3});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::corrupt_record);

  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordSequenceOffset,
                 std::byte{2});
  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordLogicalTimeOffset,
                 std::byte{4});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::corrupt_record);

  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordLogicalTimeOffset,
                 std::byte{6});
  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordReservedOffset,
                 std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error(), JournalError::corrupt_record);
}

TEST(JournalTest, AppendRejectsSequenceAndTimeDiscontinuityWithoutMutation) {
  TemporaryDirectory temporary;
  auto created = MmapJournal::create(temporary.path() / "commands.journal", 2U);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->append(command(2, 1)), JournalError::sequence_discontinuity);
  EXPECT_EQ(created->append(command(1, 5)), JournalError::none);
  EXPECT_EQ(created->append(command(2, 4)), JournalError::decreasing_logical_time);
  EXPECT_EQ(created->size(), 1U);
}

TEST(JournalTest, EndToEndAppendBeforeApplyReplaysIdentically) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(path, 4U);
  ASSERT_TRUE(journal.has_value());
  Sequencer sequencer;
  SequencedEngine live{PriceDomain{Price{100}, 11U}, 4U, Quantity{100U}};
  std::array<EngineEvent, 5> live_buffer{};
  std::array<EngineEvent, 8> live_events{};
  std::size_t live_count{};
  const std::array payloads{
      CommandPayload::submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{3}),
      CommandPayload::submit_limit(OrderId{2}, Side::buy, Price{104}, Quantity{5}),
      CommandPayload::amend_quantity(Handle{0, 2}, Quantity{1}),
      CommandPayload::cancel(Handle{0, 2}),
  };
  for (const CommandPayload& payload : payloads) {
    const auto stamped = sequencer.stamp(payload, sequencer.last_logical_time() + 1U);
    ASSERT_TRUE(stamped.has_value());
    ASSERT_EQ(journal->append(*stamped), JournalError::none);
    const ApplyResult applied = live.apply(*stamped, live_buffer);
    ASSERT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t index = 0; index < applied.event_count; ++index) {
      live_events[live_count++] = live_buffer[index];
    }
  }
  ASSERT_EQ(journal->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  SequencedEngine replay{PriceDomain{Price{100}, 11U}, 4U, Quantity{100U}};
  std::array<EngineEvent, 5> replay_buffer{};
  std::array<EngineEvent, 8> replay_events{};
  std::size_t replay_count{};
  for (std::uint64_t index = 0; index < reopened->size(); ++index) {
    const auto replayed = reopened->read(index);
    ASSERT_TRUE(replayed.has_value());
    const ApplyResult applied = replay.apply(*replayed, replay_buffer);
    ASSERT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t event_index = 0; event_index < applied.event_count; ++event_index) {
      replay_events[replay_count++] = replay_buffer[event_index];
    }
  }

  EXPECT_EQ(replay_count, live_count);
  EXPECT_TRUE(
      std::equal(live_events.begin(), live_events.begin() + live_count, replay_events.begin()));
  EXPECT_EQ(replay.order_book().best_bid(), live.order_book().best_bid());
  EXPECT_EQ(replay.order_book().best_ask(), live.order_book().best_ask());
  EXPECT_EQ(replay.order_book().level_info(Side::buy, Price{104}),
            live.order_book().level_info(Side::buy, Price{104}));
  EXPECT_EQ(replay.order_book().check_invariants(), live.order_book().check_invariants());
}

} // namespace
} // namespace matching_engine
