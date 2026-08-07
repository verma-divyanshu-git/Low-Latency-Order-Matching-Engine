#include "matching_engine/journal.hpp"

#if !defined(__APPLE__) && !defined(__linux__)
#error "matching_engine persistence supports only macOS and Linux"
#endif

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
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
  if ((status.st_mode & 0777) != 0600) {
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
    return std::unexpected{JournalError::out_of_range};
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
    : descriptor_{descriptor}, mapping_{mapping}, mapping_size_{mapping_size}, capacity_{capacity} {
}

MmapJournal::MmapJournal(MmapJournal&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      mapping_{std::exchange(other.mapping_, nullptr)},
      mapping_size_{std::exchange(other.mapping_size_, 0U)},
      capacity_{std::exchange(other.capacity_, 0U)}, size_{std::exchange(other.size_, 0U)},
      last_logical_time_{std::exchange(other.last_logical_time_, 0U)} {}

MmapJournal& MmapJournal::operator=(MmapJournal&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    descriptor_ = std::exchange(other.descriptor_, -1);
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapping_size_ = std::exchange(other.mapping_size_, 0U);
    capacity_ = std::exchange(other.capacity_, 0U);
    size_ = std::exchange(other.size_, 0U);
    last_logical_time_ = std::exchange(other.last_logical_time_, 0U);
  }
  return *this;
}

MmapJournal::~MmapJournal() {
  static_cast<void>(close());
}

std::expected<MmapJournal, JournalError> MmapJournal::create(const std::filesystem::path& path,
                                                             std::uint64_t capacity) noexcept {
  std::size_t file_size{};
  if (capacity == 0U || capacity > kMaximumJournalCapacity) {
    return std::unexpected{JournalError::invalid_capacity};
  }
  if (!checked_file_size(capacity, file_size)) {
    return std::unexpected{JournalError::file_size_overflow};
  }
  const int descriptor = ::open(path.c_str(), open_flags(true), 0600);
  if (descriptor < 0) {
    return std::unexpected{errno_error(errno)};
  }
  auto fail = [&](JournalError error) {
    static_cast<void>(::close(descriptor));
    static_cast<void>(::unlink(path.c_str()));
    return std::expected<MmapJournal, JournalError>{std::unexpected{error}};
  };
  if (::fchmod(descriptor, 0600) != 0) {
    return fail(errno_error(errno));
  }
  if (validate_regular_file(descriptor) != JournalError::none) {
    return fail(JournalError::not_regular_file);
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
  if (::msync(mapping, file_size, MS_SYNC) != 0 || ::fsync(descriptor) != 0) {
    static_cast<void>(::munmap(mapping, file_size));
    return fail(JournalError::io_error);
  }
  return MmapJournal{descriptor, mapping, file_size, capacity};
}

std::expected<MmapJournal, JournalError>
MmapJournal::open(const std::filesystem::path& path) noexcept {
  struct stat path_status{};
  if (::lstat(path.c_str(), &path_status) != 0) {
    return std::unexpected{errno_error(errno)};
  }
  if (S_ISLNK(path_status.st_mode)) {
    return std::unexpected{JournalError::symlink};
  }
  if (!S_ISREG(path_status.st_mode)) {
    return std::unexpected{JournalError::not_regular_file};
  }
  const int descriptor = ::open(path.c_str(), open_flags(false));
  if (descriptor < 0) {
    return std::unexpected{errno_error(errno)};
  }
  auto fail = [&](JournalError error) {
    static_cast<void>(::close(descriptor));
    return std::expected<MmapJournal, JournalError>{std::unexpected{error}};
  };
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
    static_cast<void>(journal.close());
    return std::unexpected{recovery};
  }
  return journal;
}

JournalError MmapJournal::recover() noexcept {
  size_ = 0U;
  last_logical_time_ = 0U;
  for (std::uint64_t index = 0U; index < capacity_; ++index) {
    const auto record = record_span(mapping_, index);
    if (read_u32(record, static_cast<std::size_t>(kRecordCommitOffset)) != kCommittedMarker) {
      return JournalError::none;
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
  if (mapping_ == nullptr || descriptor_ < 0) {
    return JournalError::closed;
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
  if (::msync(mapping_, mapping_size_, MS_SYNC) != 0 || ::fsync(descriptor_) != 0) {
    return JournalError::io_error;
  }

  write_u32(record, static_cast<std::size_t>(kRecordCommitOffset), kCommittedMarker);
  if (::msync(mapping_, mapping_size_, MS_SYNC) != 0 || ::fsync(descriptor_) != 0) {
    write_u32(record, static_cast<std::size_t>(kRecordCommitOffset), 0U);
    static_cast<void>(::msync(mapping_, mapping_size_, MS_SYNC));
    static_cast<void>(::fsync(descriptor_));
    return JournalError::io_error;
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
  JournalError result = JournalError::none;
  if (mapping_ != nullptr) {
    if (::msync(mapping_, mapping_size_, MS_SYNC) != 0) {
      result = JournalError::io_error;
    }
    if (::munmap(mapping_, mapping_size_) != 0) {
      result = JournalError::io_error;
    }
    mapping_ = nullptr;
    mapping_size_ = 0U;
  }
  if (descriptor_ >= 0) {
    if (::fsync(descriptor_) != 0) {
      result = JournalError::io_error;
    }
    if (::close(descriptor_) != 0) {
      result = JournalError::io_error;
    }
    descriptor_ = -1;
  }
  return result;
}

} // namespace matching_engine
