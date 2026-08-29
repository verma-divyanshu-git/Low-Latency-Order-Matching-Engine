#include "matching_engine/journal.hpp"

#if !defined(__APPLE__) && !defined(__linux__)
#error "matching_engine persistence supports only macOS and Linux"
#endif

#include <array>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
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

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
struct FailureScope {
  std::filesystem::path path;
  const void* identity{};
  std::uint64_t failures{};
  bool path_scoped{};
  journal_testing::ParentOpenedHook parent_opened_hook{};
  void* hook_context{};
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

void run_parent_opened_hook(const std::filesystem::path& path) noexcept {
  if (failure_scope.path != path || failure_scope.parent_opened_hook == nullptr) {
    return;
  }
  const journal_testing::ParentOpenedHook hook =
      std::exchange(failure_scope.parent_opened_hook, nullptr);
  hook(failure_scope.hook_context);
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

struct JournalPath {
  std::filesystem::path parent;
  std::filesystem::path basename;
};

[[nodiscard]] std::expected<JournalPath, JournalError>
parse_journal_path(const std::filesystem::path& path) noexcept {
  try {
    const auto& native = path.native();
    if (native.empty() || native.back() == '/' || native.find('\0') != native.npos) {
      return std::unexpected{JournalError::invalid_path};
    }
    const std::filesystem::path basename = path.filename();
    const auto& basename_native = basename.native();
    if (basename_native.empty() || basename_native == "." || basename_native == ".." ||
        basename_native.find('/') != basename_native.npos ||
        basename_native.find('\0') != basename_native.npos) {
      return std::unexpected{JournalError::invalid_path};
    }
    std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    return JournalPath{.parent = std::move(parent), .basename = basename};
  } catch (...) {
    return std::unexpected{JournalError::io_error};
  }
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

[[nodiscard]] int open_parent_directory(const JournalPath& journal_path,
                                        const std::filesystem::path& full_path) noexcept {
  if (should_fail(SyscallPoint::parent_directory_open, &full_path, nullptr)) {
    errno = EIO;
    return -1;
  }
  const int descriptor = ::open(journal_path.parent.c_str(), directory_open_flags());
  if (descriptor < 0 && errno == ENOTDIR) {
    const int open_error = errno;
    struct stat status{};
    if (::lstat(journal_path.parent.c_str(), &status) == 0 && S_ISLNK(status.st_mode)) {
      errno = ELOOP;
    } else {
      errno = open_error;
    }
  }
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
  if (descriptor >= 0) {
    run_parent_opened_hook(full_path);
  }
#endif
  return descriptor;
}

[[nodiscard]] int cleanup_unmap(std::byte* mapping, std::size_t size,
                                const std::filesystem::path& path) noexcept {
  if (should_fail(SyscallPoint::cleanup_munmap, &path, nullptr)) {
    errno = EIO;
    return -1;
  }
  return ::munmap(mapping, size);
}

[[nodiscard]] int close_descriptor(int descriptor, SyscallPoint point,
                                   const std::filesystem::path* path,
                                   const void* identity = nullptr) noexcept {
  if (should_fail(point, path, identity)) {
    errno = EIO;
    return -1;
  }
  return ::close(descriptor);
}

[[nodiscard]] int cleanup_unlink(int parent_descriptor, const JournalPath& journal_path,
                                 const std::filesystem::path& full_path) noexcept {
  if (should_fail(SyscallPoint::cleanup_unlink, &full_path, nullptr)) {
    errno = EIO;
    return -1;
  }
  return ::unlinkat(parent_descriptor, journal_path.basename.c_str(), 0);
}

[[nodiscard]] int journal_unmap(std::byte* mapping, std::size_t size, SyscallPoint point,
                                const void* identity) noexcept {
  if (should_fail(point, nullptr, identity)) {
    errno = EIO;
    return -1;
  }
  return ::munmap(mapping, size);
}

[[nodiscard]] JournalOpenFailure
cleanup_after_open_failure(JournalError operation, int parent_descriptor, int descriptor,
                           std::byte* mapping, std::size_t mapping_size,
                           const JournalPath& journal_path, const std::filesystem::path& full_path,
                           bool created) noexcept {
  JournalError cleanup = JournalError::none;
  if (mapping != nullptr && (created ? cleanup_unmap(mapping, mapping_size, full_path)
                                     : ::munmap(mapping, mapping_size)) != 0) {
    cleanup = JournalError::io_error;
  }
  if (created) {
    if (cleanup_unlink(parent_descriptor, journal_path, full_path) != 0) {
      cleanup = JournalError::io_error;
    }
    if (sync_file(parent_descriptor, SyscallPoint::cleanup_parent_fsync, &full_path, nullptr) !=
        0) {
      cleanup = JournalError::io_error;
    }
  }
  if (descriptor >= 0 &&
      close_descriptor(descriptor,
                       created ? SyscallPoint::cleanup_file_close : SyscallPoint::close_file,
                       created ? &full_path : nullptr) != 0) {
    cleanup = JournalError::io_error;
  }
  if (parent_descriptor >= 0 &&
      close_descriptor(parent_descriptor, SyscallPoint::cleanup_parent_close, &full_path) != 0) {
    cleanup = JournalError::io_error;
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
  case JournalError::invalid_path:
    return "invalid journal path";
  case JournalError::invalid_capacity:
    return "invalid capacity";
  case JournalError::invalid_base_sequence:
    return "invalid base sequence";
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
  failure_scope = {.path = path,
                   .identity = nullptr,
                   .failures = failures,
                   .path_scoped = true,
                   .parent_opened_hook = nullptr,
                   .hook_context = nullptr};
}

void journal_testing::fail_for_journal(const void* identity, std::uint64_t failures) noexcept {
  failure_scope = {.path = {},
                   .identity = identity,
                   .failures = failures,
                   .path_scoped = false,
                   .parent_opened_hook = nullptr,
                   .hook_context = nullptr};
}

void journal_testing::run_after_parent_open(const std::filesystem::path& path,
                                            ParentOpenedHook hook, void* context) noexcept {
  failure_scope.path = path;
  failure_scope.path_scoped = true;
  failure_scope.parent_opened_hook = hook;
  failure_scope.hook_context = context;
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
                         std::uint64_t capacity, std::uint64_t base_sequence) noexcept
    : descriptor_{descriptor}, mapping_{mapping}, mapping_size_{mapping_size}, capacity_{capacity},
      base_sequence_{base_sequence},
      owner_pid_{static_cast<std::int64_t>(::getpid())} {}

MmapJournal::MmapJournal(MmapJournal&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      mapping_{std::exchange(other.mapping_, nullptr)},
      mapping_size_{std::exchange(other.mapping_size_, 0U)},
      capacity_{std::exchange(other.capacity_, 0U)},
      base_sequence_{std::exchange(other.base_sequence_, 1U)},
      size_{std::exchange(other.size_, 0U)},
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
    base_sequence_ = std::exchange(other.base_sequence_, 1U);
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
MmapJournal::create(const std::filesystem::path& path, std::uint64_t capacity,
                    Sequence base_sequence) noexcept {
  std::size_t file_size{};
  if (capacity == 0U || capacity > kMaximumJournalCapacity) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::invalid_capacity}};
  }
  if (base_sequence.value() == 0U ||
      capacity - 1U > std::numeric_limits<std::uint64_t>::max() - base_sequence.value()) {
    return std::unexpected{
        JournalOpenFailure{.operation = JournalError::invalid_base_sequence}};
  }
  if (!checked_file_size(capacity, file_size)) {
    return std::unexpected{JournalOpenFailure{.operation = JournalError::file_size_overflow}};
  }
  const auto parsed = parse_journal_path(path);
  if (!parsed.has_value()) {
    return std::unexpected{JournalOpenFailure{.operation = parsed.error()}};
  }
  int parent_descriptor = open_parent_directory(*parsed, path);
  if (parent_descriptor < 0) {
    return std::unexpected{JournalOpenFailure{.operation = errno_error(errno)}};
  }
  int descriptor = ::openat(parent_descriptor, parsed->basename.c_str(), open_flags(true), 0600);
  if (descriptor < 0) {
    return std::unexpected{cleanup_after_open_failure(errno_error(errno), parent_descriptor, -1,
                                                      nullptr, 0U, *parsed, path, false)};
  }
  auto fail = [&](JournalError error, std::byte* mapping = nullptr) {
    return std::expected<MmapJournal, JournalOpenFailure>{
        std::unexpected{cleanup_after_open_failure(error, parent_descriptor, descriptor, mapping,
                                                   file_size, *parsed, path, true)}};
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
  write_u64(header, static_cast<std::size_t>(kJournalBaseSequenceOffset), base_sequence.value());
  const bool mapping_sync_failed =
      sync_mapping(mapping, file_size, SyscallPoint::create_header_msync, &path, nullptr) != 0;
  const bool file_sync_failed =
      sync_file(descriptor, SyscallPoint::create_file_fsync, &path, nullptr) != 0;
  if (mapping_sync_failed || file_sync_failed) {
    return fail(JournalError::io_error, mapping);
  }
  if (sync_file(parent_descriptor, SyscallPoint::create_parent_fsync, &path, nullptr) != 0) {
    return fail(JournalError::io_error, mapping);
  }
  if (close_descriptor(parent_descriptor, SyscallPoint::operation_parent_close, &path) != 0) {
    return fail(JournalError::io_error, mapping);
  }
  parent_descriptor = -1;
  return MmapJournal{descriptor, mapping, file_size, capacity, base_sequence.value()};
}

std::expected<MmapJournal, JournalOpenFailure>
MmapJournal::open(const std::filesystem::path& path) noexcept {
  const auto parsed = parse_journal_path(path);
  if (!parsed.has_value()) {
    return std::unexpected{JournalOpenFailure{.operation = parsed.error()}};
  }
  int parent_descriptor = open_parent_directory(*parsed, path);
  if (parent_descriptor < 0) {
    return std::unexpected{JournalOpenFailure{.operation = errno_error(errno)}};
  }
  auto fail_without_file = [&](JournalError error) {
    return std::expected<MmapJournal, JournalOpenFailure>{
        std::unexpected{cleanup_after_open_failure(error, parent_descriptor, -1, nullptr, 0U,
                                                   *parsed, path, false)}};
  };
  struct stat path_status{};
  if (::fstatat(parent_descriptor, parsed->basename.c_str(), &path_status, AT_SYMLINK_NOFOLLOW) !=
      0) {
    return fail_without_file(errno_error(errno));
  }
  if (S_ISLNK(path_status.st_mode)) {
    return fail_without_file(JournalError::symlink);
  }
  if (!S_ISREG(path_status.st_mode)) {
    return fail_without_file(JournalError::not_regular_file);
  }
  int descriptor = ::openat(parent_descriptor, parsed->basename.c_str(), open_flags(false));
  if (descriptor < 0) {
    return fail_without_file(errno_error(errno));
  }
  auto fail = [&](JournalError error, std::byte* mapping = nullptr, std::size_t mapping_size = 0U) {
    return std::expected<MmapJournal, JournalOpenFailure>{
        std::unexpected{cleanup_after_open_failure(error, parent_descriptor, descriptor, mapping,
                                                   mapping_size, *parsed, path, false)}};
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
  const std::uint32_t version = read_u32(header, 8U);
  const std::size_t reserved_offset = version == 1U ? 28U : kJournalHeaderReservedOffset;
  if ((version != 1U && version != kJournalFormatVersion) ||
      read_u32(header, 12U) != kJournalHeaderSize || read_u32(header, 16U) != kJournalRecordSize ||
      !all_zero(std::span<const std::byte>{header}.subspan(
          reserved_offset))) {
    return fail(JournalError::invalid_header);
  }
  const std::uint64_t capacity = read_u64(header, 20U);
  const std::uint64_t base_sequence =
      version == 1U ? 1U : read_u64(header, static_cast<std::size_t>(kJournalBaseSequenceOffset));
  if (base_sequence == 0U ||
      capacity == 0U || capacity - 1U > std::numeric_limits<std::uint64_t>::max() - base_sequence) {
    return fail(JournalError::invalid_header);
  }
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
  MmapJournal journal{descriptor, static_cast<std::byte*>(raw_mapping), file_size, capacity,
                      base_sequence};
  auto relinquish_journal_resources = [&journal] {
    journal.descriptor_ = -1;
    journal.mapping_ = nullptr;
    journal.mapping_size_ = 0U;
  };
  const JournalError recovery = journal.recover();
  if (recovery != JournalError::none) {
    const JournalOpenFailure failure = cleanup_after_open_failure(
        recovery, parent_descriptor, descriptor, journal.mapping_, file_size, *parsed, path, false);
    relinquish_journal_resources();
    return std::unexpected{failure};
  }
  if (close_descriptor(parent_descriptor, SyscallPoint::operation_parent_close, &path) != 0) {
    const JournalOpenFailure failure =
        cleanup_after_open_failure(JournalError::io_error, parent_descriptor, descriptor,
                                   journal.mapping_, file_size, *parsed, path, false);
    relinquish_journal_resources();
    return std::unexpected{failure};
  }
  parent_descriptor = -1;
  return journal;
}

JournalError MmapJournal::recover() noexcept {
  size_ = 0U;
  last_logical_time_ = 0U;
  for (std::uint64_t index = 0U; index < capacity_; ++index) {
    const auto record = record_span(mapping_, index);
    const std::uint32_t marker = read_u32(record, static_cast<std::size_t>(kRecordCommitOffset));
    if (marker == 0U) {
      for (std::uint64_t trailing = index + 1U; trailing < capacity_; ++trailing) {
        const auto trailing_record = record_span(mapping_, trailing);
        if (read_u32(trailing_record, static_cast<std::size_t>(kRecordCommitOffset)) != 0U) {
          return JournalError::corrupt_record;
        }
      }
      return JournalError::none;
    }
    if (marker != kCommittedMarker) {
      return JournalError::corrupt_record;
    }
    const auto decoded = decode_record(record);
    if (!decoded.has_value() || decoded->sequence.value() != base_sequence_ + index ||
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
  if (command.sequence.value() != base_sequence_ + size_) {
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
    if (mapping_ != nullptr) {
      if (::munmap(mapping_, mapping_size_) != 0) {
        result = JournalError::io_error;
      } else {
        mapping_ = nullptr;
        mapping_size_ = 0U;
      }
    }
    if (descriptor_ >= 0) {
      if (::close(descriptor_) != 0) {
        result = JournalError::io_error;
      } else {
        descriptor_ = -1;
      }
    }
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
    } else {
      mapping_ = nullptr;
      mapping_size_ = 0U;
    }
  }
  if (descriptor_ >= 0) {
    if (sync_file(descriptor_, SyscallPoint::close_file_fsync, nullptr, identity) != 0) {
      result = JournalError::io_error;
    }
    if (close_descriptor(descriptor_, SyscallPoint::close_file, nullptr, identity) != 0) {
      result = JournalError::io_error;
    } else {
      descriptor_ = -1;
    }
  }
  return result;
}

RotatingJournal::RotatingJournal(std::filesystem::path path_prefix,
                                 std::uint64_t segment_capacity,
                                 MmapJournal active) noexcept
    : path_prefix_{std::move(path_prefix)}, segment_capacity_{segment_capacity},
      active_{std::move(active)} {}

std::expected<std::filesystem::path, JournalError>
RotatingJournal::segment_path(const std::filesystem::path& path_prefix,
                              Sequence base_sequence) noexcept {
  try {
    if (path_prefix.empty() || base_sequence.value() == 0U) {
      return std::unexpected{JournalError::invalid_path};
    }
    std::string sequence = std::to_string(base_sequence.value());
    sequence.insert(0U, 20U - sequence.size(), '0');
    return std::filesystem::path{path_prefix.string() + "." + sequence + ".journal"};
  } catch (...) {
    return std::unexpected{JournalError::io_error};
  }
}

std::expected<RotatingJournal, JournalOpenFailure>
RotatingJournal::create(const std::filesystem::path& path_prefix,
                        std::uint64_t segment_capacity, Sequence base_sequence) noexcept {
  const auto path = segment_path(path_prefix, base_sequence);
  if (!path.has_value()) {
    return std::unexpected{JournalOpenFailure{.operation = path.error()}};
  }
  auto journal = MmapJournal::create(*path, segment_capacity, base_sequence);
  if (!journal.has_value()) {
    return std::unexpected{journal.error()};
  }
  return RotatingJournal{path_prefix, segment_capacity, std::move(*journal)};
}

JournalError RotatingJournal::rotate() noexcept {
  if (!active_.has_value()) {
    return JournalError::closed;
  }
  const std::uint64_t next_base =
      active_->base_sequence().value() + active_->size();
  const JournalError close_error = active_->close();
  if (close_error != JournalError::none) {
    return close_error;
  }
  const auto path = segment_path(path_prefix_, Sequence{next_base});
  if (!path.has_value()) {
    return path.error();
  }
  auto next = MmapJournal::create(*path, segment_capacity_, Sequence{next_base});
  if (!next.has_value()) {
    return next.error().operation;
  }
  active_ = std::move(*next);
  ++segment_count_;
  return JournalError::none;
}

JournalError RotatingJournal::append(const SequencedCommand& command) noexcept {
  if (!active_.has_value()) {
    return JournalError::closed;
  }
  if (validate_command_payload(command.payload) != CommandValidationError::none) {
    return JournalError::invalid_command;
  }
  if (command.sequence.value() !=
      active_->base_sequence().value() + active_->size()) {
    return JournalError::sequence_discontinuity;
  }
  if (command.logical_time < last_logical_time_) {
    return JournalError::decreasing_logical_time;
  }
  if (active_->full()) {
    const JournalError rotation = rotate();
    if (rotation != JournalError::none) {
      return rotation;
    }
  }
  const JournalError result = active_->append(command);
  if (result == JournalError::none) {
    last_logical_time_ = command.logical_time;
  }
  return result;
}

JournalError RotatingJournal::close() noexcept {
  if (!active_.has_value()) {
    return JournalError::none;
  }
  const JournalError result = active_->close();
  if (result == JournalError::none) {
    active_.reset();
  }
  return result;
}

const char* journal_set_error_message(JournalSetError error) noexcept {
  switch (error) {
  case JournalSetError::none:
    return "none";
  case JournalSetError::invalid_path:
    return "invalid journal segment prefix";
  case JournalSetError::io_error:
    return "journal segment I/O error";
  case JournalSetError::malformed_segment_name:
    return "malformed journal segment name";
  case JournalSetError::no_segments:
    return "no journal segments";
  case JournalSetError::journal_open:
    return "journal segment failed validation";
  case JournalSetError::sequence_gap:
    return "journal segment sequence gap";
  case JournalSetError::sequence_overlap:
    return "journal segment sequence overlap";
  case JournalSetError::nonfinal_partial_segment:
    return "non-final journal segment is partial";
  }
  return "unknown journal segment error";
}

JournalSegmentSet::JournalSegmentSet(std::vector<std::filesystem::path> paths,
                                     std::vector<MmapJournal> segments) noexcept
    : paths_{std::move(paths)}, segments_{std::move(segments)} {}

std::expected<JournalSegmentSet, JournalSetError>
JournalSegmentSet::open(const std::filesystem::path& path_prefix) noexcept {
  try {
    const std::filesystem::path basename = path_prefix.filename();
    if (path_prefix.empty() || basename.empty() || basename == "." || basename == "..") {
      return std::unexpected{JournalSetError::invalid_path};
    }
    std::filesystem::path parent = path_prefix.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    const std::string name_prefix = basename.string() + ".";
    constexpr std::string_view suffix = ".journal";
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
      const std::string name = entry.path().filename().string();
      if (!name.starts_with(name_prefix) || !name.ends_with(suffix)) {
        continue;
      }
      const std::size_t digits_begin = name_prefix.size();
      const std::size_t digits_size = name.size() - digits_begin - suffix.size();
      if (digits_size != 20U) {
        return std::unexpected{JournalSetError::malformed_segment_name};
      }
      std::uint64_t base{};
      const char* const begin = name.data() + digits_begin;
      const char* const end = begin + digits_size;
      const auto parsed = std::from_chars(begin, end, base);
      if (parsed.ec != std::errc{} || parsed.ptr != end || base == 0U) {
        return std::unexpected{JournalSetError::malformed_segment_name};
      }
      candidates.emplace_back(base, entry.path());
    }
    if (candidates.empty()) {
      return std::unexpected{JournalSetError::no_segments};
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
      return left.first < right.first;
    });

    std::vector<std::filesystem::path> paths;
    std::vector<MmapJournal> segments;
    paths.reserve(candidates.size());
    segments.reserve(candidates.size());
    for (const auto& [named_base, path] : candidates) {
      auto segment = MmapJournal::open(path);
      if (!segment.has_value()) {
        return std::unexpected{JournalSetError::journal_open};
      }
      if (segment->base_sequence().value() != named_base) {
        return std::unexpected{JournalSetError::malformed_segment_name};
      }
      if (!segments.empty()) {
        MmapJournal& previous = segments.back();
        const std::uint64_t expected =
            previous.base_sequence().value() + previous.size();
        if (!previous.full()) {
          return std::unexpected{JournalSetError::nonfinal_partial_segment};
        }
        if (named_base < expected) {
          return std::unexpected{JournalSetError::sequence_overlap};
        }
        if (named_base > expected) {
          return std::unexpected{JournalSetError::sequence_gap};
        }
      }
      paths.push_back(path);
      segments.push_back(std::move(*segment));
    }
    return JournalSegmentSet{std::move(paths), std::move(segments)};
  } catch (...) {
    return std::unexpected{JournalSetError::io_error};
  }
}

} // namespace matching_engine
