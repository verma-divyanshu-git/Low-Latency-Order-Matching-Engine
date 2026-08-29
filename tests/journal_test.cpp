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

struct ParentSwapContext {
  std::filesystem::path original_parent;
  std::filesystem::path moved_parent;
  std::filesystem::path decoy_path;
  bool succeeded{};
};

void swap_parent_and_create_decoy(void* raw_context) noexcept {
  auto& context = *static_cast<ParentSwapContext*>(raw_context);
  if (::rename(context.original_parent.c_str(), context.moved_parent.c_str()) != 0 ||
      ::mkdir(context.original_parent.c_str(), 0700) != 0) {
    return;
  }
  const int decoy =
      ::open(context.decoy_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (decoy < 0) {
    return;
  }
  constexpr std::byte marker{0x5a};
  const bool written =
      ::write(decoy, &marker, sizeof(marker)) == static_cast<ssize_t>(sizeof(marker));
  const bool closed = ::close(decoy) == 0;
  context.succeeded = written && closed;
}

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

TEST(JournalTest, PersistsNonDefaultBaseSequenceAcrossRecovery) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "rotated.journal";
  auto created = MmapJournal::create(path, 3U, Sequence{41U});
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->base_sequence(), Sequence{41U});
  EXPECT_EQ(created->append(command(40U, 1U)), JournalError::sequence_discontinuity);
  EXPECT_EQ(created->append(command(41U, 1U)), JournalError::none);
  EXPECT_EQ(created->append(command(42U, 2U)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->base_sequence(), Sequence{41U});
  EXPECT_EQ(reopened->size(), 2U);
  EXPECT_EQ(*reopened->read(0U), command(41U, 1U));
  EXPECT_EQ(*reopened->read(1U), command(42U, 2U));
  EXPECT_EQ(reopened->append(command(43U, 3U)), JournalError::none);
}

TEST(JournalTest, RejectsInvalidBaseSequenceAndSequenceRangeOverflow) {
  TemporaryDirectory temporary;
  EXPECT_EQ(MmapJournal::create(temporary.path() / "zero.journal", 1U, Sequence{0U})
                .error()
                .operation,
            JournalError::invalid_base_sequence);
  EXPECT_EQ(MmapJournal::create(temporary.path() / "overflow.journal", 2U,
                                Sequence{std::numeric_limits<std::uint64_t>::max()})
                .error()
                .operation,
            JournalError::invalid_base_sequence);
}

TEST(JournalTest, OpensVersionOneHeaderWithImplicitBaseSequenceOne) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "legacy.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1U, 1U)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);
  overwrite_byte(path, 8U, std::byte{1U});
  for (std::uint64_t offset = 28U; offset < 36U; ++offset) {
    overwrite_byte(path, offset, std::byte{0U});
  }

  auto reopened = MmapJournal::open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->base_sequence(), Sequence{1U});
  EXPECT_EQ(*reopened->read(0U), command(1U, 1U));
}

TEST(JournalTest, RejectsZeroBaseSequenceInVersionTwoHeader) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "invalid-base.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->close(), JournalError::none);
  for (std::uint64_t offset = kJournalBaseSequenceOffset;
       offset < kJournalBaseSequenceOffset + 8U; ++offset) {
    overwrite_byte(path, offset, std::byte{0U});
  }

  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::invalid_header);
}

TEST(RotatingJournalTest, CreatesDeterministicContiguousSegments) {
  TemporaryDirectory temporary;
  const auto prefix = temporary.path() / "commands";
  auto rotating = RotatingJournal::create(prefix, 2U, Sequence{10U});
  ASSERT_TRUE(rotating.has_value());
  EXPECT_EQ(rotating->active_base_sequence(), Sequence{10U});
  EXPECT_EQ(rotating->append(command(10U, 5U)), JournalError::none);
  EXPECT_EQ(rotating->append(command(11U, 5U)), JournalError::none);
  EXPECT_EQ(rotating->append(command(13U, 6U)), JournalError::sequence_discontinuity);
  EXPECT_EQ(rotating->segment_count(), 1U);
  EXPECT_EQ(rotating->append(command(12U, 4U)), JournalError::decreasing_logical_time);
  EXPECT_EQ(rotating->segment_count(), 1U);
  EXPECT_EQ(rotating->append(command(12U, 6U)), JournalError::none);
  EXPECT_EQ(rotating->active_base_sequence(), Sequence{12U});
  EXPECT_EQ(rotating->segment_count(), 2U);
  ASSERT_EQ(rotating->close(), JournalError::none);

  const auto first_path = RotatingJournal::segment_path(prefix, Sequence{10U});
  const auto second_path = RotatingJournal::segment_path(prefix, Sequence{12U});
  ASSERT_TRUE(first_path.has_value());
  ASSERT_TRUE(second_path.has_value());
  auto first = MmapJournal::open(*first_path);
  auto second = MmapJournal::open(*second_path);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->base_sequence(), Sequence{10U});
  EXPECT_EQ(first->size(), 2U);
  EXPECT_EQ(*first->read(0U), command(10U, 5U));
  EXPECT_EQ(*first->read(1U), command(11U, 5U));
  EXPECT_EQ(second->base_sequence(), Sequence{12U});
  EXPECT_EQ(second->size(), 1U);
  EXPECT_EQ(*second->read(0U), command(12U, 6U));
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

TEST(JournalTest, RejectsMaliciousOrAmbiguousBasenames) {
  TemporaryDirectory temporary;
  ScopedCurrentPath current_path{temporary.path()};
  const std::string embedded_nul{"bad\0name", 8U};
  const std::array<std::filesystem::path, 7> invalid_paths{
      "", ".", "..", "commands.journal/", "nested/.", "nested/..", embedded_nul};

  for (const auto& path : invalid_paths) {
    const auto created = MmapJournal::create(path, 1U);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().operation, JournalError::invalid_path);

    const auto opened = MmapJournal::open(path);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().operation, JournalError::invalid_path);
  }
}

TEST(JournalTest, RetainedParentIdentitySurvivesRenameAndControlsCleanup) {
  TemporaryDirectory temporary;
  const auto original_parent = temporary.path() / "original";
  const auto moved_parent = temporary.path() / "moved";
  ASSERT_TRUE(std::filesystem::create_directory(original_parent));
  const auto path = original_parent / "commands.journal";
  ParentSwapContext context{
      .original_parent = original_parent, .moved_parent = moved_parent, .decoy_path = path};
  journal_testing::fail_for_path(
      path, journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync));
  journal_testing::run_after_parent_open(path, swap_parent_and_create_decoy, &context);

  const auto created = MmapJournal::create(path, 1U);

  ASSERT_TRUE(context.succeeded);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().operation, JournalError::io_error);
  EXPECT_EQ(created.error().cleanup, JournalError::none);
  EXPECT_FALSE(std::filesystem::exists(moved_parent / "commands.journal"));
  EXPECT_EQ(std::filesystem::file_size(path), 1U);
}

TEST(JournalTest, OpenUsesJournalFromRetainedParentAfterRename) {
  TemporaryDirectory temporary;
  const auto original_parent = temporary.path() / "original";
  const auto moved_parent = temporary.path() / "moved";
  ASSERT_TRUE(std::filesystem::create_directory(original_parent));
  const auto path = original_parent / "commands.journal";
  auto created = MmapJournal::create(path, 1U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);
  ParentSwapContext context{
      .original_parent = original_parent, .moved_parent = moved_parent, .decoy_path = path};
  journal_testing::run_after_parent_open(path, swap_parent_and_create_decoy, &context);

  auto opened = MmapJournal::open(path);

  ASSERT_TRUE(context.succeeded);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->size(), 1U);
  EXPECT_EQ(*opened->read(0U), command(1, 1));
  EXPECT_EQ(std::filesystem::file_size(path), 1U);
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

  const auto real_parent = temporary.path() / "real-parent";
  const auto linked_parent = temporary.path() / "linked-parent";
  ASSERT_TRUE(std::filesystem::create_directory(real_parent));
  ASSERT_EQ(::symlink(real_parent.c_str(), linked_parent.c_str()), 0);
  EXPECT_EQ(MmapJournal::create(linked_parent / "commands.journal", 1U).error().operation,
            JournalError::symlink);
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

TEST(JournalTest, RejectsCommittedRecordAfterZeroGapWithoutParsingPayload) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 3U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->append(command(2, 2)), JournalError::none);
  ASSERT_EQ(created->append(command(3, 3)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);
  for (std::uint64_t index = 0U; index < 4U; ++index) {
    overwrite_byte(path, kJournalHeaderSize + kJournalRecordSize + index, std::byte{0});
  }
  overwrite_byte(path, kJournalHeaderSize + 2U * kJournalRecordSize + kRecordPayloadOffset,
                 std::byte{0xff});

  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
}

TEST(JournalTest, RejectsMalformedMarkerAfterZeroGap) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 3U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);
  overwrite_byte(path, kJournalHeaderSize + 2U * kJournalRecordSize, std::byte{0x7f});

  EXPECT_EQ(MmapJournal::open(path).error().operation, JournalError::corrupt_record);
}

TEST(JournalTest, AcceptsAllZeroSlotsAfterCleanEnd) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto created = MmapJournal::create(path, 3U);
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(created->append(command(1, 1)), JournalError::none);
  ASSERT_EQ(created->close(), JournalError::none);

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

TEST(JournalTest, ParentDirectoryOpenAndCloseFailuresAreExplicit) {
  TemporaryDirectory temporary;
  const auto open_path = temporary.path() / "open-failure.journal";
  journal_testing::fail_for_path(
      open_path,
      journal_testing::failure_mask(journal_testing::FailurePoint::parent_directory_open));
  const auto open_failed = MmapJournal::create(open_path, 1U);
  ASSERT_FALSE(open_failed.has_value());
  EXPECT_EQ(open_failed.error().operation, JournalError::io_error);
  EXPECT_EQ(open_failed.error().cleanup, JournalError::none);
  EXPECT_FALSE(std::filesystem::exists(open_path));

  const auto close_path = temporary.path() / "close-failure.journal";
  journal_testing::fail_for_path(
      close_path,
      journal_testing::failure_mask(journal_testing::FailurePoint::operation_parent_close));
  const auto close_failed = MmapJournal::create(close_path, 1U);
  ASSERT_FALSE(close_failed.has_value());
  EXPECT_EQ(close_failed.error().operation, JournalError::io_error);
  EXPECT_EQ(close_failed.error().cleanup, JournalError::none);
  EXPECT_FALSE(std::filesystem::exists(close_path));

  const auto existing_path = temporary.path() / "existing.journal";
  auto existing = MmapJournal::create(existing_path, 1U);
  ASSERT_TRUE(existing.has_value());
  ASSERT_EQ(existing->close(), JournalError::none);
  journal_testing::fail_for_path(
      existing_path,
      journal_testing::failure_mask(journal_testing::FailurePoint::operation_parent_close));
  const auto existing_close_failed = MmapJournal::open(existing_path);
  ASSERT_FALSE(existing_close_failed.has_value());
  EXPECT_EQ(existing_close_failed.error().operation, JournalError::io_error);
  EXPECT_EQ(existing_close_failed.error().cleanup, JournalError::none);
  EXPECT_TRUE(MmapJournal::open(existing_path).has_value());
}

TEST(JournalTest, CleanupUnlinkFailureRetainsCreatedPathForExplicitTeardown) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  const std::uint64_t failures =
      journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync) |
      journal_testing::failure_mask(journal_testing::FailurePoint::cleanup_unlink);
  journal_testing::fail_for_path(path, failures);

  const auto created = MmapJournal::create(path, 1U);

  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().operation, JournalError::io_error);
  EXPECT_EQ(created.error().cleanup, JournalError::io_error);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::remove(path));
}

TEST(JournalTest, CleanupDirectorySyncFailureDoesNotLeakResources) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  const std::uint64_t failures =
      journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync) |
      journal_testing::failure_mask(journal_testing::FailurePoint::cleanup_parent_fsync);
  journal_testing::fail_for_path(path, failures);

  const auto created = MmapJournal::create(path, 1U);

  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().operation, JournalError::io_error);
  EXPECT_EQ(created.error().cleanup, JournalError::io_error);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(JournalTest, LeakingCleanupFailuresAreIsolatedToChildProcess) {
  TemporaryDirectory temporary;
  const std::array cleanup_failures{
      journal_testing::FailurePoint::cleanup_munmap,
      journal_testing::FailurePoint::cleanup_file_close,
      journal_testing::FailurePoint::cleanup_parent_close,
  };
  for (std::size_t index = 0U; index < cleanup_failures.size(); ++index) {
    const auto path = temporary.path() / ("commands-" + std::to_string(index) + ".journal");
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      const std::uint64_t failures =
          journal_testing::failure_mask(journal_testing::FailurePoint::create_parent_fsync) |
          journal_testing::failure_mask(cleanup_failures[index]);
      journal_testing::fail_for_path(path, failures);
      const auto created = MmapJournal::create(path, 1U);
      const bool expected = !created.has_value() &&
                            created.error().operation == JournalError::io_error &&
                            created.error().cleanup == JournalError::io_error;
      ::_exit(expected ? 0 : 1);
    }

    int status{};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
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
    EXPECT_EQ(created->close(), JournalError::none);
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
