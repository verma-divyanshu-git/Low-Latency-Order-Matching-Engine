#include "matching_engine/journal.hpp"

#if !defined(__APPLE__) && !defined(__linux__)
#error "matching_engine persistence supports only macOS and Linux"
#endif

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace matching_engine {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{'M'}, std::byte{'E'}, std::byte{'J'},
                                          std::byte{'N'}, std::byte{'L'}, std::byte{'4'},
                                          std::byte{'A'}, std::byte{0}};
constexpr std::uint32_t kCommittedMarker = 0x54494d43U;

enum class SyscallPoint : std::uint8_t {
  create_header_msync,
  create_file_fsync,
  create_parent_fsync,
  append_pre_publish_msync,
  append_pre_publish_fsync,
  append_post_publish_msync,
  append_post_publish_fsync,
  cleanup_munmap,
  cleanup_close,
  cleanup_unlink,
  cleanup_parent_fsync,
  close_msync,
  close_munmap,
  close_file_fsync,
  close_file,
};

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
struct FailureScope {
  std::filesystem::path path;
  const void* identity{};
  std::uint64_t failures{};
  bool path_scoped{};
};

thread_local FailureScope failure_scope;

[[nodiscard]] bool consume_failure(SyscallPoint point, const std::filesystem::path* path,
                                   const void* identity) noexcept {
  const std::uint64_t bit = std::uint64_t{1U} << static_cast<std::uint8_t>(point);
  const bool scope_matches = failure_scope.path_scoped
                                 ? path != nullptr && failure_scope.path == *path
                                 : identity != nullptr && failure_scope.identity == identity;
  if (!scope_matches || (failure_scope.failures & bit) == 0U) {
    return false;
  }
  failure_scope.failures &= ~bit;
  return true;
}
#endif

[[nodiscard]] bool should_fail(SyscallPoint point, const std::filesystem::path* path,
                               const void* identity) noexcept {
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
  return consume_failure(point, path, identity);
#else
  static_cast<void>(point);
  static_cast<void>(path);
  static_cast<void>(identity);
  return false;
#endif
}

[[nodiscard]] int sync_mapping(std::byte* mapping, std::size_t size, SyscallPoint point,
                               const std::filesystem::path* path, const void* identity) noexcept {
  if (should_fail(point, path, identity)) {
    errno = EIO;
    return -1;
  }
  return ::msync(mapping, size, MS_SYNC);
}

[[nodiscard]] int sync_file(int descriptor, SyscallPoint point, const std::filesystem::path* path,
                            const void* identity) noexcept {
  if (should_fail(point, path, identity)) {
    errno = EIO;
    return -1;
  }
  return ::fsync(descriptor);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4U; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) noexcept {
  for (const std::byte value : bytes) {
    if (value != std::byte{0}) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] JournalError errno_error(int error) noexcept {
  if (error == EEXIST) {
    return JournalError::already_exists;
  }
  if (error == ENOENT) {
    return JournalError::not_found;
  }
  if (error == EACCES || error == EPERM) {
    return JournalError::permission_denied;
  }
#ifdef ELOOP
  if (error == ELOOP) {
    return JournalError::symlink;
  }
#endif
  return JournalError::io_error;
}

[[nodiscard]] JournalError acquire_ownership_lock(int descriptor) noexcept {
  if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
    return JournalError::none;
  }
  return errno == EWOULDBLOCK || errno == EAGAIN ? JournalError::locked : errno_error(errno);
}

[[nodiscard]] std::filesystem::path parent_directory(const std::filesystem::path& path) {
  const std::filesystem::path parent = path.parent_path();
  return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] int directory_open_flags() noexcept {
  int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

[[nodiscard]] bool sync_parent_directory(const std::filesystem::path& path,
                                         SyscallPoint point) noexcept {
  const std::filesystem::path parent = parent_directory(path);
  const int descriptor = ::open(parent.c_str(), directory_open_flags());
  if (descriptor < 0) {
    return false;
  }
  const bool sync_failed = sync_file(descriptor, point, &path, nullptr) != 0;
  const bool close_failed = ::close(descriptor) != 0;
  return !sync_failed && !close_failed;
}

[[nodiscard]] int cleanup_unmap(std::byte* mapping, std::size_t size,
                                const std::filesystem::path& path) noexcept {
  const bool injected = should_fail(SyscallPoint::cleanup_munmap, &path, nullptr);
  const int result = ::munmap(mapping, size);
  return injected ? -1 : result;
}

[[nodiscard]] int cleanup_close(int descriptor, const std::filesystem::path& path) noexcept {
  const bool injected = should_fail(SyscallPoint::cleanup_close, &path, nullptr);
  const int result = ::close(descriptor);
  return injected ? -1 : result;
}

[[nodiscard]] int cleanup_unlink(const std::filesystem::path& path) noexcept {
  const bool injected = should_fail(SyscallPoint::cleanup_unlink, &path, nullptr);
  const int result = ::unlink(path.c_str());
  return injected ? -1 : result;
}

[[nodiscard]] int journal_unmap(std::byte* mapping, std::size_t size, SyscallPoint point,
                                const void* identity) noexcept {
  const bool injected = should_fail(point, nullptr, identity);
  const int result = ::munmap(mapping, size);
  return injected ? -1 : result;
}

[[nodiscard]] int journal_close(int descriptor, SyscallPoint point, const void* identity) noexcept {
  const bool injected = should_fail(point, nullptr, identity);
  const int result = ::close(descriptor);
  return injected ? -1 : result;
}

[[nodiscard]] JournalOpenFailure
cleanup_after_open_failure(JournalError operation, int descriptor, std::byte* mapping,
                           std::size_t mapping_size,
                           const std::filesystem::path* created_path) noexcept {
  JournalError cleanup = JournalError::none;
  if (mapping != nullptr &&
      (created_path == nullptr ? ::munmap(mapping, mapping_size)
                               : cleanup_unmap(mapping, mapping_size, *created_path)) != 0) {
    cleanup = JournalError::io_error;
  }
  if (descriptor >= 0 &&
      (created_path == nullptr ? ::close(descriptor) : cleanup_close(descriptor, *created_path)) !=
          0) {
    cleanup = JournalError::io_error;
  }
  if (created_path != nullptr) {
    if (cleanup_unlink(*created_path) != 0) {
      cleanup = JournalError::io_error;
    }
    if (!sync_parent_directory(*created_path, SyscallPoint::cleanup_parent_fsync)) {
      cleanup = JournalError::io_error;
    }
  }
  return {.operation = operation, .cleanup = cleanup};
}

[[nodiscard]] bool checked_file_size(std::uint64_t capacity, std::size_t& size) noexcept {
  if (capacity == 0U || capacity > kMaximumJournalCapacity) {
    return false;
  }
  if (capacity >
      (std::numeric_limits<std::uint64_t>::max() - kJournalHeaderSize) / kJournalRecordSize) {
    return false;
  }
  const std::uint64_t wide_size = kJournalHeaderSize + capacity * kJournalRecordSize;
  if (wide_size > std::numeric_limits<std::size_t>::max() ||
      wide_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return false;
  }
  size = static_cast<std::size_t>(wide_size);
  return true;
}

[[nodiscard]] int open_flags(bool create) noexcept {
  int flags = O_RDWR | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  if (create) {
    flags |= O_CREAT | O_EXCL;
  }
  return flags;
}

[[nodiscard]] JournalError validate_regular_file(int descriptor) noexcept {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    return errno_error(errno);
  }
  if (!S_ISREG(status.st_mode)) {
    return JournalError::not_regular_file;
  }
  if ((status.st_mode & 07777) != 0600) {
    return JournalError::permission_denied;
  }
  return JournalError::none;
}

[[nodiscard]] bool read_exact(int descriptor, std::span<std::byte> output) noexcept {
  std::size_t completed{};
  while (completed < output.size()) {
    const ssize_t result = ::pread(descriptor, output.data() + completed, output.size() - completed,
                                   static_cast<off_t>(completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

[[nodiscard]] std::span<std::byte> record_span(std::byte* mapping, std::uint64_t index) noexcept {
  const auto offset = static_cast<std::size_t>(kJournalHeaderSize + index * kJournalRecordSize);
  return {mapping + offset, static_cast<std::size_t>(kJournalRecordSize)};
}

[[nodiscard]] std::span<const std::byte> record_span(const std::byte* mapping,
                                                     std::uint64_t index) noexcept {
  const auto offset = static_cast<std::size_t>(kJournalHeaderSize + index * kJournalRecordSize);
  return {mapping + offset, static_cast<std::size_t>(kJournalRecordSize)};
}

[[nodiscard]] std::expected<SequencedCommand, JournalError>
decode_record(std::span<const std::byte> record) noexcept {
  if (read_u32(record, static_cast<std::size_t>(kRecordCommitOffset)) != kCommittedMarker) {
    return std::unexpected{JournalError::corrupt_record};
  }
  if (read_u32(record, static_cast<std::size_t>(kRecordPayloadLengthOffset)) !=
          kEncodedCommandPayloadSize ||
      !all_zero(record.subspan(static_cast<std::size_t>(kRecordReservedOffset)))) {
    return std::unexpected{JournalError::corrupt_record};
  }
  const auto protected_bytes =
      record.subspan(static_cast<std::size_t>(kRecordSequenceOffset),
                     static_cast<std::size_t>(kRecordCrcOffset - kRecordSequenceOffset));
  if (crc32c(protected_bytes) != read_u32(record, static_cast<std::size_t>(kRecordCrcOffset))) {
    return std::unexpected{JournalError::corrupt_record};
  }
  const auto payload_bytes =
      record.subspan(static_cast<std::size_t>(kRecordPayloadOffset), kEncodedCommandPayloadSize);
  const auto payload = decode_command_payload(payload_bytes);
  if (!payload.has_value()) {
    return std::unexpected{JournalError::corrupt_record};
  }
  return SequencedCommand{
      .payload = *payload,
      .sequence = Sequence{read_u64(record, static_cast<std::size_t>(kRecordSequenceOffset))},
      .logical_time = read_u64(record, static_cast<std::size_t>(kRecordLogicalTimeOffset))};
}

} // namespace

const char* journal_error_message(JournalError error) noexcept {
  switch (error) {
  case JournalError::none:
    return "none";
  case JournalError::invalid_capacity:
    return "invalid capacity";
  case JournalError::already_exists:
    return "path already exists";
  case JournalError::not_found:
    return "path not found";
  case JournalError::permission_denied:
    return "permission denied or mode is not 0600";
  case JournalError::symlink:
    return "symlink rejected";
  case JournalError::not_regular_file:
    return "not a regular file";
  case JournalError::file_size_overflow:
    return "file size overflow";
  case JournalError::file_size_mismatch:
    return "file size mismatch";
  case JournalError::invalid_header:
    return "invalid journal header";
  case JournalError::unsupported_platform:
    return "unsupported platform";
  case JournalError::map_failed:
    return "mmap failed";
  case JournalError::io_error:
    return "I/O error";
  case JournalError::locked:
    return "journal is owned by another open writer";
  case JournalError::commit_indeterminate:
    return "commit durability is indeterminate; close and recover";
  case JournalError::writer_poisoned:
    return "writer is poisoned; close and recover";
  case JournalError::wrong_process:
    return "journal belongs to a different process";
  case JournalError::corrupt_record:
    return "corrupt committed record";
  case JournalError::full:
    return "journal full";
  case JournalError::invalid_command:
    return "invalid command";
  case JournalError::sequence_discontinuity:
    return "sequence discontinuity";
  case JournalError::decreasing_logical_time:
    return "decreasing logical time";
  case JournalError::out_of_range:
    return "record index out of range";
  case JournalError::closed:
    return "journal closed";
  }
  return "unknown journal error";
}

JournalFailureMessages journal_failure_messages(JournalOpenFailure failure) noexcept {
  return {.operation = journal_error_message(failure.operation),
          .cleanup = journal_error_message(failure.cleanup)};
}

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
void journal_testing::fail_for_path(const std::filesystem::path& path,
                                    std::uint64_t failures) noexcept {
  failure_scope = {.path = path, .identity = nullptr, .failures = failures, .path_scoped = true};
}

void journal_testing::fail_for_journal(const void* identity, std::uint64_t failures) noexcept {
  failure_scope = {.path = {}, .identity = identity, .failures = failures, .path_scoped = false};
}
#endif

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte value : bytes) {
    crc ^= std::to_integer<std::uint8_t>(value);
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~crc;
}

MmapJournal::MmapJournal(int descriptor, std::byte* mapping, std::size_t mapping_size,
                         std::uint64_t capacity) noexcept
    : descriptor_{descriptor}, mapping_{mapping}, mapping_size_{mapping_size}, capacity_{capacity},
      owner_pid_{static_cast<std::int64_t>(::getpid())} {}

MmapJournal::MmapJournal(MmapJournal&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      mapping_{std::exchange(other.mapping_, nullptr)},
      mapping_size_{std::exchange(other.mapping_size_, 0U)},
      capacity_{std::exchange(other.capacity_, 0U)}, size_{std::exchange(other.size_, 0U)},
      last_logical_time_{std::exchange(other.last_logical_time_, 0U)},
      owner_pid_{std::exchange(other.owner_pid_, 0)},
      writer_poisoned_{std::exchange(other.writer_poisoned_, false)} {}

MmapJournal& MmapJournal::operator=(MmapJournal&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    descriptor_ = std::exchange(other.descriptor_, -1);
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapping_size_ = std::exchange(other.mapping_size_, 0U);
    capacity_ = std::exchange(other.capacity_, 0U);
    size_ = std::exchange(other.size_, 0U);
    last_logical_time_ = std::exchange(other.last_logical_time_, 0U);
    owner_pid_ = std::exchange(other.owner_pid_, 0);
    writer_poisoned_ = std::exchange(other.writer_poisoned_, false);
  }
  return *this;
}

MmapJournal::~MmapJournal() {
  static_cast<void>(close());
}

std::expected<MmapJournal, JournalOpenFailure>
MmapJournal::create(const std::filesystem::path& path, std::uint64_t capacity) noexcept {
  std::size_t file_size{};
  if (capacity == 0U || capacity > kMaximumJournalCapacity) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::invalid_capacity}};
  }
  if (!checked_file_size(capacity, file_size)) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::file_size_overflow}};
  }
  const int descriptor = ::open(path.c_str(), open_flags(true), 0600);
  if (descriptor < 0) {
    return std::unexpected{JournalOpenFailure{.operation = errno_error(errno)}};
  }
  auto fail = [&](JournalError error, std::byte* mapping = nullptr) {
    return std::expected<MmapJournal, JournalOpenFailure>{
        std::unexpected{cleanup_after_open_failure(error, descriptor, mapping, file_size, &path)}};
  };
  const JournalError lock = acquire_ownership_lock(descriptor);
  if (lock != JournalError::none) {
    return fail(lock);
  }
  if (::fchmod(descriptor, 0600) != 0) {
    return fail(errno_error(errno));
  }
  const JournalError regular = validate_regular_file(descriptor);
  if (regular != JournalError::none) {
    return fail(regular);
  }
  if (::ftruncate(descriptor, static_cast<off_t>(file_size)) != 0) {
    return fail(errno_error(errno));
  }
  void* const raw_mapping =
      ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  if (raw_mapping == MAP_FAILED) {
    return fail(JournalError::map_failed);
  }
  auto* const mapping = static_cast<std::byte*>(raw_mapping);
  for (std::size_t index = 0U; index < file_size; ++index) {
    mapping[index] = std::byte{0};
  }
  for (std::size_t index = 0U; index < kMagic.size(); ++index) {
    mapping[index] = kMagic[index];
  }
  const std::span<std::byte> header{mapping, static_cast<std::size_t>(kJournalHeaderSize)};
  write_u32(header, 8U, kJournalFormatVersion);
  write_u32(header, 12U, static_cast<std::uint32_t>(kJournalHeaderSize));
  write_u32(header, 16U, static_cast<std::uint32_t>(kJournalRecordSize));
  write_u64(header, 20U, capacity);
  const bool mapping_sync_failed =
      sync_mapping(mapping, file_size, SyscallPoint::create_header_msync, &path, nullptr) != 0;
  const bool file_sync_failed =
      sync_file(descriptor, SyscallPoint::create_file_fsync, &path, nullptr) != 0;
  if (mapping_sync_failed || file_sync_failed) {
    return fail(JournalError::io_error, mapping);
  }
  if (!sync_parent_directory(path, SyscallPoint::create_parent_fsync)) {
    return fail(JournalError::io_error, mapping);
  }
  return MmapJournal{descriptor, mapping, file_size, capacity};
}

std::expected<MmapJournal, JournalOpenFailure>
MmapJournal::open(const std::filesystem::path& path) noexcept {
  struct stat path_status{};
  if (::lstat(path.c_str(), &path_status) != 0) {
    return std::unexpected{JournalOpenFailure{.operation = errno_error(errno)}};
  }
  if (S_ISLNK(path_status.st_mode)) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::symlink}};
  }
  if (!S_ISREG(path_status.st_mode)) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::not_regular_file}};
  }
  const int descriptor = ::open(path.c_str(), open_flags(false));
  if (descriptor < 0) {
    return std::unexpected{JournalOpenFailure{.operation = errno_error(errno)}};
  }
  auto fail = [&](JournalError error, std::byte* mapping = nullptr, std::size_t mapping_size = 0U) {
    return std::expected<MmapJournal, JournalOpenFailure>{std::unexpected{
        cleanup_after_open_failure(error, descriptor, mapping, mapping_size, nullptr)}};
  };
  const JournalError lock = acquire_ownership_lock(descriptor);
  if (lock != JournalError::none) {
    return fail(lock);
  }
  const JournalError regular = validate_regular_file(descriptor);
  if (regular != JournalError::none) {
    return fail(regular);
  }
  std::array<std::byte, kJournalHeaderSize> header{};
  if (!read_exact(descriptor, header)) {
    return fail(JournalError::invalid_header);
  }
  for (std::size_t index = 0U; index < kMagic.size(); ++index) {
    if (header[index] != kMagic[index]) {
      return fail(JournalError::invalid_header);
    }
  }
  if (read_u32(header, 8U) != kJournalFormatVersion ||
      read_u32(header, 12U) != kJournalHeaderSize || read_u32(header, 16U) != kJournalRecordSize ||
      !all_zero(std::span<const std::byte>{header}.subspan(
          static_cast<std::size_t>(kJournalHeaderReservedOffset)))) {
    return fail(JournalError::invalid_header);
  }
  const std::uint64_t capacity = read_u64(header, 20U);
  std::size_t file_size{};
  if (!checked_file_size(capacity, file_size)) {
    return fail(JournalError::invalid_header);
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    return fail(errno_error(errno));
  }
  if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != file_size) {
    return fail(JournalError::file_size_mismatch);
  }
  void* const raw_mapping =
      ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  if (raw_mapping == MAP_FAILED) {
    return fail(JournalError::map_failed);
  }
  MmapJournal journal{descriptor, static_cast<std::byte*>(raw_mapping), file_size, capacity};
  const JournalError recovery = journal.recover();
  if (recovery != JournalError::none) {
    const JournalOpenFailure failure =
        cleanup_after_open_failure(recovery, descriptor, journal.mapping_, file_size, nullptr);
    journal.descriptor_ = -1;
    journal.mapping_ = nullptr;
    journal.mapping_size_ = 0U;
    return std::unexpected{failure};
  }
  return journal;
}

JournalError MmapJournal::recover() noexcept {
  size_ = 0U;
  last_logical_time_ = 0U;
  for (std::uint64_t index = 0U; index < capacity_; ++index) {
    const auto record = record_span(mapping_, index);
    const std::uint32_t marker = read_u32(record, static_cast<std::size_t>(kRecordCommitOffset));
    if (marker == 0U) {
      return JournalError::none;
    }
    if (marker != kCommittedMarker) {
      return JournalError::corrupt_record;
    }
    const auto decoded = decode_record(record);
    if (!decoded.has_value() || decoded->sequence.value() != index + 1U ||
        decoded->logical_time < last_logical_time_) {
      return JournalError::corrupt_record;
    }
    size_ = index + 1U;
    last_logical_time_ = decoded->logical_time;
  }
  return JournalError::none;
}

JournalError MmapJournal::append(const SequencedCommand& command) noexcept {
  if (static_cast<std::int64_t>(::getpid()) != owner_pid_) {
    return JournalError::wrong_process;
  }
  if (mapping_ == nullptr || descriptor_ < 0) {
    return JournalError::closed;
  }
  if (writer_poisoned_) {
    return JournalError::writer_poisoned;
  }
  if (size_ == capacity_) {
    return JournalError::full;
  }
  if (validate_command_payload(command.payload) != CommandValidationError::none) {
    return JournalError::invalid_command;
  }
  if (command.sequence.value() != size_ + 1U) {
    return JournalError::sequence_discontinuity;
  }
  if (command.logical_time < last_logical_time_) {
    return JournalError::decreasing_logical_time;
  }

  std::span<std::byte> record = record_span(mapping_, size_);
  for (std::byte& value : record) {
    value = std::byte{0};
  }
  write_u64(record, static_cast<std::size_t>(kRecordSequenceOffset), command.sequence.value());
  write_u64(record, static_cast<std::size_t>(kRecordLogicalTimeOffset), command.logical_time);
  write_u32(record, static_cast<std::size_t>(kRecordPayloadLengthOffset),
            static_cast<std::uint32_t>(kEncodedCommandPayloadSize));
  const auto payload =
      record.subspan(static_cast<std::size_t>(kRecordPayloadOffset), kEncodedCommandPayloadSize);
  if (encode_command_payload(command.payload, payload) != CommandCodecError::none) {
    return JournalError::invalid_command;
  }
  const auto protected_bytes = std::span<const std::byte>{record}.subspan(
      static_cast<std::size_t>(kRecordSequenceOffset),
      static_cast<std::size_t>(kRecordCrcOffset - kRecordSequenceOffset));
  write_u32(record, static_cast<std::size_t>(kRecordCrcOffset), crc32c(protected_bytes));
  const bool data_mapping_sync_failed =
      sync_mapping(mapping_, mapping_size_, SyscallPoint::append_pre_publish_msync, nullptr,
                   this) != 0;
  const bool data_file_sync_failed =
      sync_file(descriptor_, SyscallPoint::append_pre_publish_fsync, nullptr, this) != 0;
  if (data_mapping_sync_failed || data_file_sync_failed) {
    return JournalError::io_error;
  }

  write_u32(record, static_cast<std::size_t>(kRecordCommitOffset), kCommittedMarker);
  const bool commit_mapping_sync_failed =
      sync_mapping(mapping_, mapping_size_, SyscallPoint::append_post_publish_msync, nullptr,
                   this) != 0;
  const bool commit_file_sync_failed =
      sync_file(descriptor_, SyscallPoint::append_post_publish_fsync, nullptr, this) != 0;
  if (commit_mapping_sync_failed || commit_file_sync_failed) {
    writer_poisoned_ = true;
    return JournalError::commit_indeterminate;
  }
  ++size_;
  last_logical_time_ = command.logical_time;
  return JournalError::none;
}

std::expected<SequencedCommand, JournalError>
MmapJournal::read(std::uint64_t index) const noexcept {
  if (mapping_ == nullptr) {
    return std::unexpected{JournalError::closed};
  }
  if (index >= size_) {
    return std::unexpected{JournalError::out_of_range};
  }
  return decode_record(record_span(static_cast<const std::byte*>(mapping_), index));
}

JournalError MmapJournal::close() noexcept {
  if (mapping_ == nullptr && descriptor_ < 0) {
    return JournalError::none;
  }
  if (static_cast<std::int64_t>(::getpid()) != owner_pid_) {
    JournalError result = JournalError::wrong_process;
    if (mapping_ != nullptr && ::munmap(mapping_, mapping_size_) != 0) {
      result = JournalError::io_error;
    }
    if (descriptor_ >= 0 && ::close(descriptor_) != 0) {
      result = JournalError::io_error;
    }
    mapping_ = nullptr;
    mapping_size_ = 0U;
    descriptor_ = -1;
    return result;
  }

  JournalError result = JournalError::none;
  const void* const identity = this;
  if (mapping_ != nullptr) {
    if (sync_mapping(mapping_, mapping_size_, SyscallPoint::close_msync, nullptr, identity) != 0) {
      result = JournalError::io_error;
    }
    if (journal_unmap(mapping_, mapping_size_, SyscallPoint::close_munmap, identity) != 0) {
      result = JournalError::io_error;
    }
    mapping_ = nullptr;
    mapping_size_ = 0U;
  }
  if (descriptor_ >= 0) {
    if (sync_file(descriptor_, SyscallPoint::close_file_fsync, nullptr, identity) != 0) {
      result = JournalError::io_error;
    }
    if (journal_close(descriptor_, SyscallPoint::close_file, identity) != 0) {
      result = JournalError::io_error;
    }
    descriptor_ = -1;
  }
  return result;
}

} // namespace matching_engine
