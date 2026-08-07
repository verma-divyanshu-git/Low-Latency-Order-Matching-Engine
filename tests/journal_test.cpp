#include "matching_engine/journal.hpp"
#include "matching_engine/sequenced_engine.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <sys/wait.h>
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

class ScopedCurrentPath {
public:
  explicit ScopedCurrentPath(const std::filesystem::path& path)
      : previous_{std::filesystem::current_path()} {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentPath() {
    std::error_code error;
    std::filesystem::current_path(previous_, error);
  }

private:
  std::filesystem::path previous_;
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
                                             std::uint64_t record_index, std::size_t record_offset,
                                             std::byte value) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(descriptor, 0);
  std::array<std::byte, kJournalRecordSize> record{};
  const auto file_offset =
      static_cast<off_t>(kJournalHeaderSize + record_index * kJournalRecordSize);
  ASSERT_EQ(::pread(descriptor, record.data(), record.size(), file_offset),
            static_cast<ssize_t>(record.size()));
  record[record_offset] = value;
  const std::uint32_t crc = crc32c(std::span<const std::byte>{record}.subspan(
      static_cast<std::size_t>(kRecordSequenceOffset),
      static_cast<std::size_t>(kRecordCrcOffset - kRecordSequenceOffset)));
  for (std::size_t index = 0U; index < 4U; ++index) {
    record[static_cast<std::size_t>(kRecordCrcOffset) + index] =
        static_cast<std::byte>((crc >> (index * 8U)) & 0xffU);
  }
  ASSERT_EQ(::pwrite(descriptor, record.data(), record.size(), file_offset),
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
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->capacity(), 3U);
  EXPECT_EQ(created->size(), 0U);
  EXPECT_EQ(created->append(command(1, 10)), JournalError::none);
  EXPECT_EQ(created->append(command(2, 10)), JournalError::none);
  EXPECT_EQ(created->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->size(), 2U);
  ASSERT_TRUE(reopened->read(0U).has_value());
  EXPECT_EQ(*reopened->read(0U), command(1, 10));
  EXPECT_EQ(*reopened->read(1U), command(2, 10));
  EXPECT_EQ(reopened->read(2U).error(), JournalError::out_of_range);
  EXPECT_EQ(reopened->append(command(3, 11)), JournalError::none);
  EXPECT_EQ(reopened->append(command(4, 12)), JournalError::full);
}

TEST(JournalTest, RetainsExclusiveNonblockingOwnershipUntilClose) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto owner = MmapJournal::create(path, 1U);
  ASSERT_TRUE(owner.has_value());

  const auto second = MmapJournal::open(path);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().operation, JournalError::locked);

  ASSERT_EQ(owner->close(), JournalError::none);
  auto successor = MmapJournal::open(path);
  ASSERT_TRUE(successor.has_value());
}

TEST(JournalTest, ForkedChildCannotAppendThroughInheritedOwner) {
  TemporaryDirectory temporary;
  auto owner = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(owner.has_value());
  int child_result[2]{};
  ASSERT_EQ(::pipe(child_result), 0);

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(::close(child_result[0]));
    const auto result = static_cast<std::uint8_t>(owner->append(command(1, 1)));
    const ssize_t written = ::write(child_result[1], &result, sizeof(result));
    static_cast<void>(written);
    ::_exit(0);
  }

  ASSERT_EQ(::close(child_result[1]), 0);
  std::uint8_t result{};
  ASSERT_EQ(::read(child_result[0], &result, sizeof(result)), static_cast<ssize_t>(sizeof(result)));
  ASSERT_EQ(::close(child_result[0]), 0);
  int status{};
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(static_cast<JournalError>(result), JournalError::wrong_process);
  EXPECT_EQ(owner->size(), 0U);
  EXPECT_EQ(owner->append(command(1, 1)), JournalError::none);
}

TEST(JournalTest, CreatesRelativePathWithoutExplicitParent) {
  TemporaryDirectory temporary;
  ScopedCurrentPath current_path{temporary.path()};

  auto created = MmapJournal::create("commands.journal", 1U);

  ASSERT_TRUE(created.has_value());
  EXPECT_TRUE(std::filesystem::exists("commands.journal"));
}

TEST(JournalTest, RejectsInvalidCapacityAndExistingPathWithoutMutation) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  EXPECT_EQ(MmapJournal::create(path, 0U).error().operation, JournalError::invalid_capacity);
  EXPECT_EQ(MmapJournal::create(path, kMaximumJournalCapacity + 1U).error().operation,
            JournalError::invalid_capacity);
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(MmapJournal::create(path, 1U).error().operation, JournalError::already_exists);
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
  EXPECT_EQ(MmapJournal::open(link).error().operation, JournalError::symlink);
  EXPECT_EQ(MmapJournal::open(temporary.path()).error().operation, JournalError::not_regular_file);
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
    EXPECT_EQ(opened.error().operation, JournalError::permission_denied);
  }
}

TEST(JournalTest, RejectsSpecialPermissionBits) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->close(), JournalError::none);
  ASSERT_EQ(::chmod(path.c_str(), 04600), 0);
  struct stat status{};
  ASSERT_EQ(::stat(path.c_str(), &status), 0);
  if ((status.st_mode & 04000) != 0) {
    EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::permission_denied);
  }
}

TEST(JournalTest, RejectsHeaderCorruptionReservedBytesAndExactSizeMismatch) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderReservedOffset, std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::invalid_header);
  overwrite_byte(path, kJournalHeaderReservedOffset, std::byte{0});
  ASSERT_EQ(::truncate(path.c_str(), static_cast<off_t>(kJournalHeaderSize)), 0);
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::file_size_mismatch);
}

TEST(JournalTest, ReportsCommittedPayloadBitFlipAsCorruption) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderSize + kRecordPayloadOffset + 4U, std::byte{0xff});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
}

TEST(JournalTest, RejectsCommittedNoncanonicalPayloadEvenWithValidCrc) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_record_byte_and_recompute_crc(
      path, 0U, static_cast<std::size_t>(kRecordPayloadOffset) + 3U, std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
}

TEST(JournalTest, AcceptsOnlyExactZeroAsCleanEndMarker) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 2)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  for (std::uint64_t index = 0U; index < 4U; ++index) {
    overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + index, std::byte{0});
  }
  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->size(), 1U);
}

TEST(JournalTest, RejectsMalformedNonzeroCommitMarker) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 2)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize, std::byte{0});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
}

TEST(JournalTest, RejectsCommittedBadSequenceLogicalTimeAndReservedBytes) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 5)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 6)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  overwrite_record_byte_and_recompute_crc(path, 1U, static_cast<std::size_t>(kRecordSequenceOffset),
                                          std::byte{3});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);

  overwrite_record_byte_and_recompute_crc(path, 1U, static_cast<std::size_t>(kRecordSequenceOffset),
                                          std::byte{2});
  overwrite_record_byte_and_recompute_crc(
      path, 1U, static_cast<std::size_t>(kRecordLogicalTimeOffset), std::byte{4});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);

  overwrite_record_byte_and_recompute_crc(
      path, 1U, static_cast<std::size_t>(kRecordLogicalTimeOffset), std::byte{6});
  overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + kRecordReservedOffset,
                 std::byte{1});
  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
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

TEST(JournalTest, PrePublishSyncFailureIsDefinitelyUncommittedAndRetryable) {
  TemporaryDirectory temporary;
  auto created = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(created.has_value());
  journal_testing::fail_for_journal(
      &*created,
      journal_testing::failure_mask(journal_testing::FailurePoint::append_pre_publish_msync));

  EXPECT_EQ(created->append(command(1, 1)), JournalError::io_error);
  EXPECT_EQ(created->size(), 0U);
  EXPECT_EQ(created->append(command(1, 1)), JournalError::none);
}

TEST(JournalTest, PrePublishFileSyncFailureIsDefinitelyUncommittedAndRetryable) {
  TemporaryDirectory temporary;
  auto created = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(created.has_value());
  journal_testing::fail_for_journal(
      &*created,
      journal_testing::failure_mask(journal_testing::FailurePoint::append_pre_publish_fsync));

  EXPECT_EQ(created->append(command(1, 1)), JournalError::io_error);
  EXPECT_EQ(created->size(), 0U);
  EXPECT_EQ(created->append(command(1, 1)), JournalError::none);
}

TEST(JournalTest, PostPublishSyncFailurePoisonsWriterUntilRecovery) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 2U);
  ASSERT_TRUE(created.has_value());
  journal_testing::fail_for_journal(
      &*created,
      journal_testing::failure_mask(journal_testing::FailurePoint::append_post_publish_msync));

  EXPECT_EQ(created->append(command(1, 1)), JournalError::commit_indeterminate);
  EXPECT_EQ(created->append(command(1, 1)), JournalError::writer_poisoned);
  ASSERT_EQ(created->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->size(), 1U);
  EXPECT_EQ(*reopened->read(0U), command(1, 1));
  EXPECT_EQ(reopened->append(command(2, 2)), JournalError::none);
}

TEST(JournalTest, PostPublishFileSyncFailurePoisonsWriterUntilRecovery) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  journal_testing::fail_for_journal(
      &*created,
      journal_testing::failure_mask(journal_testing::FailurePoint::append_post_publish_fsync));

  EXPECT_EQ(created->append(command(1, 1)), JournalError::commit_indeterminate);
  EXPECT_EQ(created->append(command(1, 1)), JournalError::writer_poisoned);
}

TEST(JournalTest, FailedCreationRemovesPathAndReportsCleanupSeparately) {
  TemporaryDirectory temporary;
  const std::array failures{journal_testing::FailurePoint::create_header_msync,
                            journal_testing::FailurePoint::create_file_fsync};
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    const auto path = temporary.path() / ("commands-" + std::to_string(index) + ".journal");
    journal_testing::fail_for_path(path, journal_testing::failure_mask(failures[index]));

    const auto created = MmapJournal::create(path, 1U);

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().operation, JournalError::io_error);
    EXPECT_EQ(created.error().cleanup, JournalError::none);
    EXPECT_FALSE(std::filesystem::exists(path));
  }
}

TEST(JournalTest, ParentDirectorySyncFailureCleansUpCreatedEntry) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  journal_testing::fail_for_path(
      path, journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync));

  const auto created = MmapJournal::create(path, 1U);

  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().operation, JournalError::io_error);
  EXPECT_EQ(created.error().cleanup, JournalError::none);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(JournalTest, CreationCleanupReportsEveryFailedSyscallBranch) {
  TemporaryDirectory temporary;
  const std::array cleanup_failures{
      journal_testing::FailurePoint::cleanup_munmap,
      journal_testing::FailurePoint::cleanup_close,
      journal_testing::FailurePoint::cleanup_unlink,
      journal_testing::FailurePoint::cleanup_parent_fsync,
  };
  for (std::size_t index = 0U; index < cleanup_failures.size(); ++index) {
    const auto path = temporary.path() / ("commands-" + std::to_string(index) + ".journal");
    const std::uint64_t failures =
        journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync) |
        journal_testing::failure_mask(cleanup_failures[index]);
    journal_testing::fail_for_path(path, failures);

    const auto created = MmapJournal::create(path, 1U);

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().operation, JournalError::io_error);
    EXPECT_EQ(created.error().cleanup, JournalError::io_error);
  }
}

TEST(JournalTest, OpenFailureMessagesExposePrimaryAndCleanup) {
  const JournalFailureMessages messages = journal_failure_messages(
      {.operation = JournalError::invalid_header, .cleanup = JournalError::io_error});

  EXPECT_STREQ(messages.operation, "invalid journal header");
  EXPECT_STREQ(messages.cleanup, "I/O error");
}

TEST(JournalTest, CloseReportsInjectedSyncAndCleanupFailures) {
  TemporaryDirectory temporary;
  const std::array failures{
      journal_testing::FailurePoint::close_msync,
      journal_testing::FailurePoint::close_munmap,
      journal_testing::FailurePoint::close_file_fsync,
      journal_testing::FailurePoint::close_file,
  };
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    auto created =
        MmapJournal::create(temporary.path() / ("close-" + std::to_string(index) + ".journal"), 1U);
    ASSERT_TRUE(created.has_value());
    journal_testing::fail_for_journal(&*created, journal_testing::failure_mask(failures[index]));
    EXPECT_EQ(created->close(), JournalError::io_error);
  }
}

TEST(JournalTest, EndToEndAppendBeforeApplyReplaysIdentically) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(path, 8U);
  ASSERT_TRUE(journal.has_value());
  Sequencer sequencer;
  SequencedEngine live{PriceDomain{Price{100}, 11U}, 6U, Quantity{100U}};
  std::array<EngineEvent, 7> live_buffer{};
  std::array<EngineEvent, 24> live_events{};
  std::size_t live_count{};
  auto append_apply = [&](const CommandPayload& payload) {
    const auto stamped = sequencer.stamp(payload, sequencer.last_logical_time() + 1U);
    EXPECT_TRUE(stamped.has_value());
    if (!stamped.has_value()) {
      return ApplyResult{ApplyStatus::invalid_command, 0U};
    }
    EXPECT_EQ(journal->append(*stamped), JournalError::none);
    const ApplyResult applied = live.apply(*stamped, live_buffer);
    EXPECT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t index = 0U; index < applied.event_count; ++index) {
      live_events[live_count++] = live_buffer[index];
    }
    return applied;
  };

  ASSERT_EQ(
      append_apply(CommandPayload::submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{3}))
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      append_apply(CommandPayload::submit_limit(OrderId{2}, Side::buy, Price{101}, Quantity{5}))
          .status,
      ApplyStatus::applied);
  const Handle original_bid = live_buffer[0].handle;
  ASSERT_EQ(append_apply(CommandPayload::submit_market(OrderId{3}, Side::buy, Quantity{2})).status,
            ApplyStatus::applied);
  ASSERT_EQ(append_apply(CommandPayload::submit_limit(OrderId{4}, Side::buy, Price{104},
                                                      Quantity{2}, TimeInForce::fok))
                .status,
            ApplyStatus::applied);
  EXPECT_EQ(live_buffer[0].reason, static_cast<std::uint8_t>(RejectReason::fok_not_fillable));
  ASSERT_EQ(append_apply(CommandPayload::cancel(Handle{kInvalidIndex, 99U})).status,
            ApplyStatus::applied);
  EXPECT_EQ(live_buffer[0].reason, static_cast<std::uint8_t>(CancelReason::invalid_handle));
  ASSERT_EQ(append_apply(CommandPayload::replace(original_bid, Price{102}, Quantity{4})).status,
            ApplyStatus::applied);
  const Handle replacement_bid = live_buffer[0].handle;
  ASSERT_NE(replacement_bid, original_bid);
  ASSERT_EQ(
      append_apply(CommandPayload::submit_limit(OrderId{5}, Side::sell, Price{102}, Quantity{2}))
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      append_apply(CommandPayload::submit_limit(OrderId{6}, Side::sell, Price{106}, Quantity{4}))
          .status,
      ApplyStatus::applied);
  const Handle retained_ask = live_buffer[0].handle;
  const std::array active_handles{replacement_bid, retained_ask};
  const std::array active_info{live.order_book().order_info(replacement_bid),
                               live.order_book().order_info(retained_ask)};
  ASSERT_TRUE(active_info[0].has_value());
  ASSERT_TRUE(active_info[1].has_value());
  ASSERT_EQ(journal->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  SequencedEngine replay{PriceDomain{Price{100}, 11U}, 6U, Quantity{100U}};
  std::array<EngineEvent, 7> replay_buffer{};
  std::array<EngineEvent, 24> replay_events{};
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
  for (std::size_t index = 0U; index < live_count; ++index) {
    EXPECT_EQ(replay_events[index], live_events[index]);
    const auto replay_bytes =
        std::bit_cast<std::array<std::byte, sizeof(EngineEvent)>>(replay_events[index]);
    const auto live_bytes =
        std::bit_cast<std::array<std::byte, sizeof(EngineEvent)>>(live_events[index]);
    EXPECT_EQ(replay_bytes, live_bytes);
  }
  EXPECT_EQ(replay.order_book().best_bid(), live.order_book().best_bid());
  EXPECT_EQ(replay.order_book().best_ask(), live.order_book().best_ask());
  for (const Handle handle : active_handles) {
    EXPECT_EQ(replay.order_book().order_info(handle), live.order_book().order_info(handle));
  }
  for (std::int64_t ticks = 100; ticks <= 110; ++ticks) {
    EXPECT_EQ(replay.order_book().level_info(Side::buy, Price{ticks}),
              live.order_book().level_info(Side::buy, Price{ticks}));
    EXPECT_EQ(replay.order_book().level_info(Side::sell, Price{ticks}),
              live.order_book().level_info(Side::sell, Price{ticks}));
  }
  EXPECT_EQ(replay.order_book().check_invariants(), live.order_book().check_invariants());
}

} // namespace
} // namespace matching_engine
