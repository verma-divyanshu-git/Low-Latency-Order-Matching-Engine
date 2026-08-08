#include "matching_engine/snapshot.hpp"

#include "matching_engine/journal.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace matching_engine {
namespace {

constexpr std::array<std::byte, 8U> kMagic{std::byte{'M'}, std::byte{'E'}, std::byte{'S'},
                                           std::byte{'N'}, std::byte{'A'}, std::byte{'P'},
                                           std::byte{'4'}, std::byte{0}};
constexpr std::size_t kChecksumOffset = 96U;
constexpr std::size_t kHeaderReservedOffset = 100U;
constexpr unsigned kTempAttempts = 16U;
std::atomic<std::uint64_t> temp_counter{};

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
struct FailureScope {
  std::filesystem::path path;
  std::uint64_t failures{};
};
thread_local FailureScope failure_scope;

bool should_fail(const std::filesystem::path& path, snapshot_testing::FailurePoint point) noexcept {
  const std::uint64_t bit = snapshot_testing::failure_mask(point);
  if (failure_scope.path != path || (failure_scope.failures & bit) == 0U) {
    return false;
  }
  failure_scope.failures &= ~bit;
  errno = EIO;
  return true;
}
#endif

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

bool zero(std::span<const std::byte> bytes) noexcept {
  for (const std::byte value : bytes) {
    if (value != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::uint32_t snapshot_crc(std::span<const std::byte> bytes) noexcept {
  std::uint32_t state = 0xffffffffU;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index >= kChecksumOffset && index < kChecksumOffset + 4U) {
      continue;
    }
    state ^= std::to_integer<std::uint8_t>(bytes[index]);
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (state & 1U);
      state = (state >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~state;
}

SnapshotError errno_error(int value) noexcept {
  if (value == ENOENT) {
    return SnapshotError::not_found;
  }
  if (value == EACCES || value == EPERM) {
    return SnapshotError::permission_denied;
  }
#ifdef ELOOP
  if (value == ELOOP) {
    return SnapshotError::symlink;
  }
#endif
  return SnapshotError::io_error;
}

struct ParsedPath {
  std::filesystem::path parent;
  std::filesystem::path basename;
};

std::expected<ParsedPath, SnapshotError> parse_path(const std::filesystem::path& path) {
  try {
    const auto& native = path.native();
    const auto basename = path.filename();
    const auto& name = basename.native();
    if (native.empty() || native.back() == '/' || native.find('\0') != native.npos ||
        name.empty() || name == "." || name == ".." || name.find('/') != name.npos ||
        name.find('\0') != name.npos) {
      return std::unexpected{SnapshotError::invalid_path};
    }
    auto parent = path.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    return ParsedPath{std::move(parent), basename};
  } catch (...) {
    return std::unexpected{SnapshotError::io_error};
  }
}

int parent_flags() noexcept {
  int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

int file_flags() noexcept {
  int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

bool write_exact(int descriptor, std::span<const std::byte> bytes) noexcept {
  std::size_t completed{};
  while (completed < bytes.size()) {
    const ssize_t result = ::write(descriptor, bytes.data() + completed, bytes.size() - completed);
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

bool read_exact(int descriptor, std::span<std::byte> bytes) noexcept {
  std::size_t completed{};
  while (completed < bytes.size()) {
    const ssize_t result = ::pread(descriptor, bytes.data() + completed, bytes.size() - completed,
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

} // namespace

namespace detail {

class SnapshotCodec {
public:
  static std::expected<std::vector<std::byte>, SnapshotError> encode(const SequencedEngine& engine,
                                                                     SnapshotPoint point) {
    const OrderBook& book = engine.order_book_;
    if (book.arena_.capacity_ > kMaximumSnapshotSlots ||
        const_cast<OrderBook&>(book).check_invariants().violation != InvariantViolation::none) {
      return std::unexpected{SnapshotError::invalid_state};
    }
    const bool terminal = engine.sequence_exhausted_;
    if ((!terminal && (point.sequence.value() == std::numeric_limits<std::uint64_t>::max() ||
                       engine.next_sequence_ != point.sequence.value() + 1U)) ||
        (terminal && (point.sequence.value() != std::numeric_limits<std::uint64_t>::max() ||
                      engine.next_sequence_ != std::numeric_limits<std::uint64_t>::max())) ||
        point.logical_time != engine.last_logical_time_) {
      return std::unexpected{SnapshotError::invalid_state};
    }
    const std::size_t size =
        kSnapshotHeaderSize + static_cast<std::size_t>(book.arena_.capacity_) * kSnapshotSlotSize;
    std::vector<std::byte> bytes(size, std::byte{0});
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
      bytes[index] = kMagic[index];
    }
    write_u32(bytes, 8U, kSnapshotFormatVersion);
    write_u32(bytes, 12U, static_cast<std::uint32_t>(kSnapshotHeaderSize));
    write_u32(bytes, 16U, static_cast<std::uint32_t>(kSnapshotSlotSize));
    write_u64(bytes, 24U, size);
    write_u64(bytes, 32U, static_cast<std::uint64_t>(book.domain_.minimum().ticks()));
    write_u32(bytes, 40U, book.domain_.tick_count());
    write_u32(bytes, 44U, book.max_order_quantity_);
    write_u32(bytes, 48U, book.arena_.capacity_);
    write_u32(bytes, 52U, book.arena_.size_);
    write_u32(bytes, 56U, book.arena_.free_head_);
    write_u32(bytes, 60U, terminal ? 1U : 0U);
    write_u64(bytes, 64U, engine.next_sequence_);
    write_u64(bytes, 72U, engine.last_logical_time_);
    write_u64(bytes, 80U, point.sequence.value());
    write_u64(bytes, 88U, point.logical_time);
    for (std::uint32_t index = 0; index < book.arena_.capacity_; ++index) {
      const auto& slot = book.arena_.slots_[index];
      const std::size_t offset =
          kSnapshotHeaderSize + static_cast<std::size_t>(index) * kSnapshotSlotSize;
      write_u32(bytes, offset, slot.generation);
      write_u32(bytes, offset + 4U, slot.free_next);
      bytes[offset + 8U] = slot.live ? std::byte{1U} : std::byte{0U};
      if (slot.live) {
        write_u64(bytes, offset + 12U, slot.order.id.value());
        write_u64(bytes, offset + 20U, slot.order.remaining.value());
        write_u32(bytes, offset + 28U, slot.order.prev_index);
        write_u32(bytes, offset + 32U, slot.order.next_index);
        write_u32(bytes, offset + 36U, slot.order.encoded_level_side);
        write_u32(bytes, offset + 40U, slot.order.reserved_flags);
      }
    }
    write_u32(bytes, kChecksumOffset, snapshot_crc(bytes));
    return bytes;
  }

  static std::expected<DecodedSnapshot, SnapshotError> decode(std::span<const std::byte> bytes) {
    if (bytes.size() < kSnapshotHeaderSize) {
      return std::unexpected{SnapshotError::invalid_length};
    }
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
      if (bytes[index] != kMagic[index]) {
        return std::unexpected{SnapshotError::invalid_header};
      }
    }
    if (read_u32(bytes, 8U) != kSnapshotFormatVersion) {
      return std::unexpected{SnapshotError::unsupported_version};
    }
    if (read_u32(bytes, 12U) != kSnapshotHeaderSize || read_u32(bytes, 16U) != kSnapshotSlotSize ||
        read_u32(bytes, 20U) != 0U ||
        !zero(bytes.subspan(kHeaderReservedOffset, kSnapshotHeaderSize - kHeaderReservedOffset))) {
      return std::unexpected{SnapshotError::invalid_header};
    }
    const std::uint32_t capacity = read_u32(bytes, 48U);
    if (capacity > kMaximumSnapshotSlots ||
        static_cast<std::size_t>(capacity) >
            (std::numeric_limits<std::size_t>::max() - kSnapshotHeaderSize) / kSnapshotSlotSize) {
      return std::unexpected{SnapshotError::file_too_large};
    }
    const std::size_t expected =
        kSnapshotHeaderSize + static_cast<std::size_t>(capacity) * kSnapshotSlotSize;
    if (bytes.size() != expected || read_u64(bytes, 24U) != expected) {
      return std::unexpected{SnapshotError::invalid_length};
    }
    if (snapshot_crc(bytes) != read_u32(bytes, kChecksumOffset)) {
      return std::unexpected{SnapshotError::checksum_mismatch};
    }
    const std::uint32_t live_count = read_u32(bytes, 52U);
    const std::uint32_t tick_count = read_u32(bytes, 40U);
    const std::uint32_t max_quantity = read_u32(bytes, 44U);
    if (live_count > capacity || tick_count == 0U || max_quantity == 0U) {
      return std::unexpected{SnapshotError::invalid_state};
    }
    const std::uint64_t next_sequence = read_u64(bytes, 64U);
    const std::uint64_t last_time = read_u64(bytes, 72U);
    const SnapshotPoint point{Sequence{read_u64(bytes, 80U)}, read_u64(bytes, 88U)};
    const std::uint32_t exhausted = read_u32(bytes, 60U);
    if (next_sequence == 0U || exhausted > 1U || point.logical_time != last_time ||
        (exhausted == 0U && (point.sequence.value() == std::numeric_limits<std::uint64_t>::max() ||
                             next_sequence != point.sequence.value() + 1U)) ||
        (exhausted == 1U && (point.sequence.value() != std::numeric_limits<std::uint64_t>::max() ||
                             next_sequence != std::numeric_limits<std::uint64_t>::max()))) {
      return std::unexpected{SnapshotError::invalid_state};
    }
    try {
      auto engine = std::make_unique<SequencedEngine>(
          PriceDomain{Price{static_cast<std::int64_t>(read_u64(bytes, 32U))}, tick_count}, capacity,
          Quantity{max_quantity}, Sequence{next_sequence}, last_time);
      engine->sequence_exhausted_ = exhausted == 1U;
      OrderBook& book = engine->order_book_;
      book.arena_.size_ = live_count;
      book.arena_.free_head_ = read_u32(bytes, 56U);
      std::uint32_t observed_live{};
      for (std::uint32_t index = 0; index < capacity; ++index) {
        const std::size_t offset =
            kSnapshotHeaderSize + static_cast<std::size_t>(index) * kSnapshotSlotSize;
        auto& slot = book.arena_.slots_[index];
        slot.generation = read_u32(bytes, offset);
        slot.free_next = read_u32(bytes, offset + 4U);
        const std::uint8_t live = std::to_integer<std::uint8_t>(bytes[offset + 8U]);
        if (slot.generation == 0U || live > 1U || !zero(bytes.subspan(offset + 9U, 3U)) ||
            read_u32(bytes, offset + 44U) != 0U) {
          return std::unexpected{SnapshotError::noncanonical_slot};
        }
        slot.live = live == 1U;
        if (!slot.live) {
          if (!zero(bytes.subspan(offset + 12U, 32U))) {
            return std::unexpected{SnapshotError::noncanonical_slot};
          }
          slot.order = {.id = OrderId{0U},
                        .remaining = Quantity{0U},
                        .prev_index = 0U,
                        .next_index = 0U,
                        .encoded_level_side = 0U,
                        .reserved_flags = 0U};
          continue;
        }
        ++observed_live;
        slot.order = {.id = OrderId{read_u64(bytes, offset + 12U)},
                      .remaining = Quantity{read_u64(bytes, offset + 20U)},
                      .prev_index = read_u32(bytes, offset + 28U),
                      .next_index = read_u32(bytes, offset + 32U),
                      .encoded_level_side = read_u32(bytes, offset + 36U),
                      .reserved_flags = read_u32(bytes, offset + 40U)};
        const std::uint32_t level = detail::decode_level(slot.order.encoded_level_side);
        if (slot.order.remaining.value() == 0U || slot.order.remaining.value() > max_quantity ||
            level >= tick_count || slot.order.reserved_flags != 0U ||
            (slot.order.prev_index != kInvalidIndex && slot.order.prev_index >= capacity) ||
            (slot.order.next_index != kInvalidIndex && slot.order.next_index >= capacity)) {
          return std::unexpected{SnapshotError::invalid_state};
        }
        PriceLevel& price_level =
            book.level(detail::decode_side(slot.order.encoded_level_side), level);
        if (price_level.aggregate_quantity >
            std::numeric_limits<std::uint64_t>::max() - slot.order.remaining.value()) {
          return std::unexpected{SnapshotError::invalid_state};
        }
        price_level.aggregate_quantity += slot.order.remaining.value();
        ++price_level.order_count;
        if (slot.order.prev_index == kInvalidIndex) {
          if (price_level.head_index != kInvalidIndex) {
            return std::unexpected{SnapshotError::invalid_state};
          }
          price_level.head_index = index;
        }
        if (slot.order.next_index == kInvalidIndex) {
          if (price_level.tail_index != kInvalidIndex) {
            return std::unexpected{SnapshotError::invalid_state};
          }
          price_level.tail_index = index;
        }
        static_cast<void>(
            book.occupancy(detail::decode_side(slot.order.encoded_level_side)).set(level));
      }
      if (observed_live != live_count) {
        return std::unexpected{SnapshotError::invalid_state};
      }
      std::vector<bool> free_seen(capacity, false);
      std::uint32_t free_count{};
      std::uint32_t current = book.arena_.free_head_;
      while (current != kInvalidIndex) {
        if (current >= capacity || book.arena_.slots_[current].live || free_seen[current]) {
          return std::unexpected{SnapshotError::invalid_state};
        }
        free_seen[current] = true;
        ++free_count;
        current = book.arena_.slots_[current].free_next;
      }
      if (free_count != capacity - live_count) {
        return std::unexpected{SnapshotError::invalid_state};
      }
      for (std::uint32_t index = 0; index < capacity; ++index) {
        if (!book.arena_.slots_[index].live && !free_seen[index]) {
          return std::unexpected{SnapshotError::invalid_state};
        }
      }
      if (book.check_invariants().violation != InvariantViolation::none) {
        return std::unexpected{SnapshotError::invalid_state};
      }
      return DecodedSnapshot{std::move(engine), point};
    } catch (...) {
      return std::unexpected{SnapshotError::invalid_state};
    }
  }
};

} // namespace detail

const char* snapshot_error_message(SnapshotError error) noexcept {
  switch (error) {
  case SnapshotError::none:
    return "none";
  case SnapshotError::invalid_path:
    return "invalid snapshot path";
  case SnapshotError::not_found:
    return "snapshot not found";
  case SnapshotError::permission_denied:
    return "permission denied or mode is not 0600";
  case SnapshotError::symlink:
    return "symlink rejected";
  case SnapshotError::not_regular_file:
    return "not a regular file";
  case SnapshotError::file_too_large:
    return "snapshot exceeds bounded size";
  case SnapshotError::invalid_length:
    return "invalid snapshot length";
  case SnapshotError::invalid_header:
    return "invalid snapshot header";
  case SnapshotError::unsupported_version:
    return "unsupported snapshot version";
  case SnapshotError::checksum_mismatch:
    return "snapshot CRC32C mismatch";
  case SnapshotError::noncanonical_slot:
    return "noncanonical snapshot slot";
  case SnapshotError::invalid_state:
    return "invalid snapshot engine state";
  case SnapshotError::io_error:
    return "snapshot I/O error";
  case SnapshotError::commit_indeterminate:
    return "snapshot replacement durability indeterminate";
  case SnapshotError::temp_collision_limit:
    return "snapshot temporary-name collision limit";
  }
  return "unknown snapshot error";
}

std::expected<std::vector<std::byte>, SnapshotError> encode_snapshot(const SequencedEngine& engine,
                                                                     SnapshotPoint point) {
  return detail::SnapshotCodec::encode(engine, point);
}

std::expected<DecodedSnapshot, SnapshotError> decode_snapshot(std::span<const std::byte> bytes) {
  return detail::SnapshotCodec::decode(bytes);
}

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
void rewrite_snapshot_crc_for_testing(std::span<std::byte> bytes) noexcept {
  if (bytes.size() >= kSnapshotHeaderSize) {
    write_u32(bytes, kChecksumOffset, snapshot_crc(bytes));
  }
}

void snapshot_testing::fail_for_path(const std::filesystem::path& path,
                                     std::uint64_t failures) noexcept {
  failure_scope = {.path = path, .failures = failures};
}
#endif

SnapshotError save_snapshot_atomic(const std::filesystem::path& path, const SequencedEngine& engine,
                                   SnapshotPoint point) {
  const auto bytes = encode_snapshot(engine, point);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  const auto parsed = parse_path(path);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  const int parent = ::open(parsed->parent.c_str(), parent_flags());
  if (parent < 0) {
    return errno_error(errno);
  }
  int descriptor = -1;
  std::string temporary;
  for (unsigned attempt = 0; attempt < kTempAttempts; ++attempt) {
    temporary = parsed->basename.string() + ".tmp." +
                std::to_string(static_cast<std::uint64_t>(::getpid())) + "." +
                std::to_string(temp_counter.fetch_add(1U, std::memory_order_relaxed));
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor = ::openat(parent, temporary.c_str(), flags, 0600);
    if (descriptor >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (descriptor < 0) {
    const SnapshotError error =
        errno == EEXIST ? SnapshotError::temp_collision_limit : errno_error(errno);
    static_cast<void>(::close(parent));
    return error;
  }
  SnapshotError result = SnapshotError::none;
  bool renamed = false;
  struct stat status{};
  if (::fchmod(descriptor, 0600) != 0 || ::fstat(descriptor, &status) != 0 ||
      !S_ISREG(status.st_mode) || (status.st_mode & 07777) != 0600 ||
      (
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
          should_fail(path, snapshot_testing::FailurePoint::write) ||
#endif
          !write_exact(descriptor, *bytes)) ||
      (
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
          should_fail(path, snapshot_testing::FailurePoint::file_fsync) ||
#endif
          ::fsync(descriptor) != 0) ||
      (
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
          should_fail(path, snapshot_testing::FailurePoint::rename) ||
#endif
          ::renameat(parent, temporary.c_str(), parent, parsed->basename.c_str()) != 0)) {
    result = SnapshotError::io_error;
  } else {
    renamed = true;
    if (
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
        should_fail(path, snapshot_testing::FailurePoint::parent_fsync) ||
#endif
        ::fsync(parent) != 0) {
      result = SnapshotError::commit_indeterminate;
    }
  }
  if (!renamed &&
      (
#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
          should_fail(path, snapshot_testing::FailurePoint::cleanup_unlink) ||
#endif
          ::unlinkat(parent, temporary.c_str(), 0) != 0) &&
      errno != ENOENT) {
    result = SnapshotError::io_error;
  }
  if (::close(descriptor) != 0 && result == SnapshotError::none) {
    result = SnapshotError::io_error;
  }
  if (::close(parent) != 0 && result == SnapshotError::none) {
    result = renamed ? SnapshotError::commit_indeterminate : SnapshotError::io_error;
  }
  return result;
}

std::expected<DecodedSnapshot, SnapshotError> load_snapshot(const std::filesystem::path& path) {
  const auto parsed = parse_path(path);
  if (!parsed.has_value()) {
    return std::unexpected{parsed.error()};
  }
  const int parent = ::open(parsed->parent.c_str(), parent_flags());
  if (parent < 0) {
    return std::unexpected{errno_error(errno)};
  }
  struct stat path_status{};
  if (::fstatat(parent, parsed->basename.c_str(), &path_status, AT_SYMLINK_NOFOLLOW) != 0) {
    const SnapshotError error = errno_error(errno);
    static_cast<void>(::close(parent));
    return std::unexpected{error};
  }
  if (S_ISLNK(path_status.st_mode)) {
    static_cast<void>(::close(parent));
    return std::unexpected{SnapshotError::symlink};
  }
  const int descriptor = ::openat(parent, parsed->basename.c_str(), file_flags());
  if (descriptor < 0) {
    const SnapshotError error = errno_error(errno);
    static_cast<void>(::close(parent));
    return std::unexpected{error};
  }
  struct stat before{};
  SnapshotError error = SnapshotError::none;
  if (::fstat(descriptor, &before) != 0) {
    error = errno_error(errno);
  } else if (!S_ISREG(before.st_mode)) {
    error = SnapshotError::not_regular_file;
  } else if ((before.st_mode & 07777) != 0600) {
    error = SnapshotError::permission_denied;
  } else if (before.st_size < 0 ||
             static_cast<std::uint64_t>(before.st_size) > kMaximumSnapshotBytes) {
    error = SnapshotError::file_too_large;
  } else if (static_cast<std::uint64_t>(before.st_size) < kSnapshotHeaderSize) {
    error = SnapshotError::invalid_length;
  }
  std::vector<std::byte> bytes;
  if (error == SnapshotError::none) {
    bytes.resize(static_cast<std::size_t>(before.st_size));
    if (!read_exact(descriptor, bytes)) {
      error = SnapshotError::io_error;
    }
  }
  struct stat after{};
  if (error == SnapshotError::none &&
      (::fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
       before.st_ino != after.st_ino || before.st_size != after.st_size)) {
    error = SnapshotError::io_error;
  }
  if (::close(descriptor) != 0 && error == SnapshotError::none) {
    error = SnapshotError::io_error;
  }
  if (::close(parent) != 0 && error == SnapshotError::none) {
    error = SnapshotError::io_error;
  }
  if (error != SnapshotError::none) {
    return std::unexpected{error};
  }
  return decode_snapshot(bytes);
}

} // namespace matching_engine
