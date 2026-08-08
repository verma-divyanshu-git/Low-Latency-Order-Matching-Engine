#ifndef MATCHING_ENGINE_SNAPSHOT_HPP
#define MATCHING_ENGINE_SNAPSHOT_HPP

#include "matching_engine/sequenced_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace matching_engine {

inline constexpr std::uint32_t kSnapshotFormatVersion = 1U;
inline constexpr std::size_t kSnapshotHeaderSize = 112U;
inline constexpr std::size_t kSnapshotSlotSize = 48U;
inline constexpr std::uint32_t kMaximumSnapshotSlots = 1'000'000U;
inline constexpr std::size_t kMaximumSnapshotBytes =
    kSnapshotHeaderSize + static_cast<std::size_t>(kMaximumSnapshotSlots) * kSnapshotSlotSize;

struct SnapshotPoint {
  Sequence sequence{0U};
  std::uint64_t logical_time{};
  constexpr bool operator==(const SnapshotPoint&) const noexcept = default;
};

enum class SnapshotError : std::uint8_t {
  none,
  invalid_path,
  not_found,
  permission_denied,
  symlink,
  not_regular_file,
  file_too_large,
  invalid_length,
  invalid_header,
  unsupported_version,
  checksum_mismatch,
  noncanonical_slot,
  invalid_state,
  io_error,
  commit_indeterminate,
  temp_collision_limit,
};

struct DecodedSnapshot {
  std::unique_ptr<SequencedEngine> engine;
  SnapshotPoint point;
};

[[nodiscard]] const char* snapshot_error_message(SnapshotError error) noexcept;
[[nodiscard]] std::expected<std::vector<std::byte>, SnapshotError>
encode_snapshot(const SequencedEngine& engine, SnapshotPoint point);
[[nodiscard]] std::expected<DecodedSnapshot, SnapshotError>
decode_snapshot(std::span<const std::byte> bytes);
[[nodiscard]] SnapshotError save_snapshot_atomic(const std::filesystem::path& path,
                                                 const SequencedEngine& engine,
                                                 SnapshotPoint point);
[[nodiscard]] std::expected<DecodedSnapshot, SnapshotError>
load_snapshot(const std::filesystem::path& path);

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
void rewrite_snapshot_crc_for_testing(std::span<std::byte> bytes) noexcept;
namespace snapshot_testing {
enum class FailurePoint : std::uint8_t {
  write,
  file_fsync,
  rename,
  parent_fsync,
  cleanup_unlink,
};
[[nodiscard]] constexpr std::uint64_t failure_mask(FailurePoint point) noexcept {
  return std::uint64_t{1U} << static_cast<std::uint8_t>(point);
}
void fail_for_path(const std::filesystem::path& path, std::uint64_t failures) noexcept;
} // namespace snapshot_testing
#endif

} // namespace matching_engine

#endif
