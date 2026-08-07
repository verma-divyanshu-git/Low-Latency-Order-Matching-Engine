#ifndef MATCHING_ENGINE_JOURNAL_HPP
#define MATCHING_ENGINE_JOURNAL_HPP

#include "matching_engine/command.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

namespace matching_engine {

inline constexpr std::uint32_t kJournalFormatVersion = 1U;
inline constexpr std::uint64_t kMaximumJournalCapacity = 1'000'000U;
inline constexpr std::uint64_t kJournalHeaderSize = 64U;
inline constexpr std::uint64_t kJournalRecordSize = 80U;
inline constexpr std::uint64_t kJournalHeaderReservedOffset = 28U;
inline constexpr std::uint64_t kRecordCommitOffset = 0U;
inline constexpr std::uint64_t kRecordSequenceOffset = 4U;
inline constexpr std::uint64_t kRecordLogicalTimeOffset = 12U;
inline constexpr std::uint64_t kRecordPayloadLengthOffset = 20U;
inline constexpr std::uint64_t kRecordPayloadOffset = 24U;
inline constexpr std::uint64_t kRecordCrcOffset = 60U;
inline constexpr std::uint64_t kRecordReservedOffset = 64U;

enum class JournalError : std::uint8_t {
  none,
  invalid_capacity,
  already_exists,
  not_found,
  permission_denied,
  symlink,
  not_regular_file,
  file_size_overflow,
  file_size_mismatch,
  invalid_header,
  unsupported_platform,
  map_failed,
  io_error,
  locked,
  commit_indeterminate,
  writer_poisoned,
  corrupt_record,
  full,
  invalid_command,
  sequence_discontinuity,
  decreasing_logical_time,
  out_of_range,
  closed,
};

struct JournalOpenFailure {
  JournalError operation{JournalError::none};
  JournalError cleanup{JournalError::none};

  constexpr bool operator==(const JournalOpenFailure&) const noexcept = default;
};

[[nodiscard]] constexpr bool operator==(JournalOpenFailure failure, JournalError error) noexcept {
  return failure.operation == error;
}

[[nodiscard]] const char* journal_error_message(JournalError error) noexcept;
[[nodiscard]] const char* journal_error_message(JournalOpenFailure failure) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
namespace journal_testing {

enum class SyncPoint : std::uint8_t {
  create_header,
  append_pre_publish,
  append_post_publish,
};

enum class CleanupPoint : std::uint8_t {
  unlink_created_path,
};

void fail_next_sync(SyncPoint point) noexcept;
void fail_next_cleanup(CleanupPoint point) noexcept;

} // namespace journal_testing
#endif

// One thread owns a journal writer. A retained advisory file lock excludes
// cooperating opens, but concurrent access remains unsupported.
class MmapJournal {
public:
  [[nodiscard]] static std::expected<MmapJournal, JournalOpenFailure>
  create(const std::filesystem::path& path, std::uint64_t capacity) noexcept;
  [[nodiscard]] static std::expected<MmapJournal, JournalOpenFailure>
  open(const std::filesystem::path& path) noexcept;

  MmapJournal(const MmapJournal&) = delete;
  MmapJournal& operator=(const MmapJournal&) = delete;
  MmapJournal(MmapJournal&& other) noexcept;
  MmapJournal& operator=(MmapJournal&& other) noexcept;
  ~MmapJournal();

  [[nodiscard]] JournalError append(const SequencedCommand& command) noexcept;
  [[nodiscard]] std::expected<SequencedCommand, JournalError>
  read(std::uint64_t index) const noexcept;
  [[nodiscard]] JournalError close() noexcept;

  [[nodiscard]] std::uint64_t capacity() const noexcept {
    return capacity_;
  }
  [[nodiscard]] std::uint64_t size() const noexcept {
    return size_;
  }

private:
  MmapJournal(int descriptor, std::byte* mapping, std::size_t mapping_size,
              std::uint64_t capacity) noexcept;
  [[nodiscard]] JournalError recover() noexcept;

  int descriptor_{-1};
  std::byte* mapping_{};
  std::size_t mapping_size_{};
  std::uint64_t capacity_{};
  std::uint64_t size_{};
  std::uint64_t last_logical_time_{};
  bool writer_poisoned_{};
};

} // namespace matching_engine

#endif
