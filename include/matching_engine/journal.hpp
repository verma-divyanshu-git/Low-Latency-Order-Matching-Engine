#ifndef MATCHING_ENGINE_JOURNAL_HPP
#define MATCHING_ENGINE_JOURNAL_HPP

#include "matching_engine/command.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>

namespace matching_engine {

inline constexpr std::uint32_t kJournalFormatVersion = 2U;
inline constexpr std::uint64_t kMaximumJournalCapacity = 1'000'000U;
inline constexpr std::uint64_t kJournalHeaderSize = 64U;
inline constexpr std::uint64_t kJournalRecordSize = 80U;
inline constexpr std::uint64_t kJournalBaseSequenceOffset = 28U;
inline constexpr std::uint64_t kJournalHeaderReservedOffset = 36U;
inline constexpr std::uint64_t kRecordCommitOffset = 0U;
inline constexpr std::uint64_t kRecordSequenceOffset = 4U;
inline constexpr std::uint64_t kRecordLogicalTimeOffset = 12U;
inline constexpr std::uint64_t kRecordPayloadLengthOffset = 20U;
inline constexpr std::uint64_t kRecordPayloadOffset = 24U;
inline constexpr std::uint64_t kRecordCrcOffset = 60U;
inline constexpr std::uint64_t kRecordReservedOffset = 64U;

enum class JournalError : std::uint8_t {
  none,
  invalid_path,
  invalid_capacity,
  invalid_base_sequence,
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
  wrong_process,
  corrupt_record,
  full,
  invalid_command,
  sequence_discontinuity,
  decreasing_logical_time,
  out_of_range,
  closed,
};

struct JournalFailureMessages {
  const char* operation;
  const char* cleanup;
};

struct JournalOpenFailure {
  JournalError operation{JournalError::none};
  JournalError cleanup{JournalError::none};

  constexpr bool operator==(const JournalOpenFailure&) const noexcept = default;
};

[[nodiscard]] const char* journal_error_message(JournalError error) noexcept;
[[nodiscard]] JournalFailureMessages journal_failure_messages(JournalOpenFailure failure) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
namespace journal_testing {

enum class FailurePoint : std::uint8_t {
  parent_directory_open,
  operation_parent_close,
  create_header_msync,
  create_file_fsync,
  create_parent_fsync,
  append_pre_publish_msync,
  append_pre_publish_fsync,
  append_post_publish_msync,
  append_post_publish_fsync,
  cleanup_munmap,
  cleanup_file_close,
  cleanup_unlink,
  cleanup_parent_fsync,
  cleanup_parent_close,
  close_msync,
  close_munmap,
  close_file_fsync,
  close_file,
};

[[nodiscard]] constexpr std::uint64_t failure_mask(FailurePoint point) noexcept {
  return std::uint64_t{1U} << static_cast<std::uint8_t>(point);
}

void fail_for_path(const std::filesystem::path& path, std::uint64_t failures) noexcept;
void fail_for_journal(const void* identity, std::uint64_t failures) noexcept;
using ParentOpenedHook = void (*)(void*) noexcept;
void run_after_parent_open(const std::filesystem::path& path, ParentOpenedHook hook,
                           void* context) noexcept;

} // namespace journal_testing
#endif

// One thread owns a journal writer. A retained advisory file lock excludes
// cooperating opens, but concurrent access remains unsupported.
class MmapJournal {
public:
  [[nodiscard]] static std::expected<MmapJournal, JournalOpenFailure>
    create(const std::filesystem::path& path, std::uint64_t capacity,
      Sequence base_sequence = Sequence{1U}) noexcept;
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
  [[nodiscard]] Sequence base_sequence() const noexcept {
    return Sequence{base_sequence_};
  }
  [[nodiscard]] bool full() const noexcept {
    return size_ == capacity_;
  }
  [[nodiscard]] bool writable() const noexcept {
    return descriptor_ >= 0 && mapping_ != nullptr && !writer_poisoned_;
  }

private:
  MmapJournal(int descriptor, std::byte* mapping, std::size_t mapping_size,
              std::uint64_t capacity, std::uint64_t base_sequence) noexcept;
  [[nodiscard]] JournalError recover() noexcept;

  int descriptor_{-1};
  std::byte* mapping_{};
  std::size_t mapping_size_{};
  std::uint64_t capacity_{};
  std::uint64_t base_sequence_{1U};
  std::uint64_t size_{};
  std::uint64_t last_logical_time_{};
  std::int64_t owner_pid_{};
  bool writer_poisoned_{};
};

class RotatingJournal {
public:
  [[nodiscard]] static std::expected<RotatingJournal, JournalOpenFailure>
  create(const std::filesystem::path& path_prefix, std::uint64_t segment_capacity,
         Sequence base_sequence = Sequence{1U}) noexcept;

  RotatingJournal(const RotatingJournal&) = delete;
  RotatingJournal& operator=(const RotatingJournal&) = delete;
  RotatingJournal(RotatingJournal&&) noexcept = default;
  RotatingJournal& operator=(RotatingJournal&&) noexcept = default;

  [[nodiscard]] JournalError append(const SequencedCommand& command) noexcept;
  [[nodiscard]] JournalError close() noexcept;

  [[nodiscard]] Sequence active_base_sequence() const noexcept {
    return active_.has_value() ? active_->base_sequence() : Sequence{0U};
  }
  [[nodiscard]] std::uint64_t segment_count() const noexcept {
    return segment_count_;
  }
  [[nodiscard]] static std::expected<std::filesystem::path, JournalError>
  segment_path(const std::filesystem::path& path_prefix, Sequence base_sequence) noexcept;

private:
  RotatingJournal(std::filesystem::path path_prefix, std::uint64_t segment_capacity,
                  MmapJournal active) noexcept;
  [[nodiscard]] JournalError rotate() noexcept;

  std::filesystem::path path_prefix_;
  std::uint64_t segment_capacity_{};
  std::optional<MmapJournal> active_;
  std::uint64_t segment_count_{1U};
  std::uint64_t last_logical_time_{};
};

} // namespace matching_engine

#endif
