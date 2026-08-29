#ifndef MATCHING_ENGINE_REPLAY_HPP
#define MATCHING_ENGINE_REPLAY_HPP

#include "matching_engine/journal.hpp"
#include "matching_engine/market_data_adapter.hpp"
#include "matching_engine/market_data_input.hpp"
#include "matching_engine/sequenced_engine.hpp"
#include "matching_engine/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace matching_engine {

inline constexpr std::size_t kEncodedEngineEventSize = 64U;

enum class EventCodecError : std::uint8_t {
  none,
  invalid_length,
  invalid_type,
  noncanonical,
};

[[nodiscard]] EventCodecError encode_engine_event(const EngineEvent& event,
                                                  std::span<std::byte> output) noexcept;
[[nodiscard]] std::expected<EngineEvent, EventCodecError>
decode_engine_event(std::span<const std::byte> input) noexcept;

// CRC32C is a compact non-cryptographic replay fingerprint only.
// Exact event comparison remains the correctness oracle.
class ReplayFingerprint {
public:
  [[nodiscard]] EventCodecError add(const EngineEvent& event) noexcept;
  [[nodiscard]] std::uint64_t event_count() const noexcept {
    return event_count_;
  }
  [[nodiscard]] std::uint64_t byte_count() const noexcept {
    return byte_count_;
  }
  [[nodiscard]] std::uint32_t crc32c() const noexcept {
    return ~crc_state_;
  }

private:
  std::uint32_t crc_state_{0xffffffffU};
  std::uint64_t event_count_{};
  std::uint64_t byte_count_{};
};

struct ReplayResult {
  std::uint64_t commands_applied{};
  Sequence first_sequence{0U};
  Sequence last_sequence{0U};
  ReplayFingerprint fingerprint{};
};

enum class ReplayError : std::uint8_t {
  journal,
  engine_state_mismatch,
  unverifiable_boundary,
  boundary_missing,
  boundary_time_mismatch,
  sequence_gap,
  apply,
  invariant,
};

enum class MarketDataReplayError : std::uint8_t {
  input,
  adapter,
  apply,
  invariant,
};

enum class JournalCompactionError : std::uint8_t {
  none,
  snapshot,
  journal_set,
  snapshot_before_retained_history,
  snapshot_ahead_of_journal,
  io_error,
  commit_indeterminate,
};

[[nodiscard]] const char* journal_compaction_error_message(JournalCompactionError error) noexcept;

[[nodiscard]] std::expected<ReplayResult, ReplayError>
replay_journal(MmapJournal& journal, SequencedEngine& engine, Sequence snapshot_sequence,
               std::uint64_t snapshot_logical_time, std::span<EngineEvent> event_buffer) noexcept;

[[nodiscard]] std::expected<ReplayResult, ReplayError>
replay_journal_segments(JournalSegmentSet& journals, SequencedEngine& engine,
                        Sequence snapshot_sequence, std::uint64_t snapshot_logical_time,
                        std::span<EngineEvent> event_buffer) noexcept;

[[nodiscard]] JournalCompactionError
compact_journal_segments(const std::filesystem::path& path_prefix,
                         const std::filesystem::path& durable_snapshot_path) noexcept;

#if defined(MATCHING_ENGINE_TEST_FAILPOINTS)
namespace replay_testing {
enum class CompactionFailurePoint : std::uint8_t {
  unlink,
  parent_fsync,
};
void fail_compaction_once(CompactionFailurePoint point) noexcept;
} // namespace replay_testing
#endif

[[nodiscard]] std::expected<ReplayResult, MarketDataReplayError>
replay_market_data(MarketDataInputStream& input, MarketDataAdapter& adapter, SequencedEngine& engine,
                   std::span<EngineEvent> event_buffer) noexcept;

} // namespace matching_engine

#endif
