#ifndef MATCHING_ENGINE_LANE_MERGE_HPP
#define MATCHING_ENGINE_LANE_MERGE_HPP

#include "matching_engine/command.hpp"
#include "matching_engine/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace matching_engine {

struct LaneCommand {
  CommandPayload payload{};
  std::uint64_t logical_time{};
  std::uint32_t lane_id{};
  std::uint64_t lane_sequence{};

  constexpr bool operator==(const LaneCommand&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<LaneCommand>);

using LaneQueue = SpscQueue<LaneCommand>;

class DeterministicLaneMerger {
public:
  explicit DeterministicLaneMerger(std::vector<LaneQueue::Consumer> consumers) noexcept
      : consumers_{std::move(consumers)} {}

  [[nodiscard]] bool try_pop(LaneCommand& command) noexcept {
    std::size_t selected{};
    bool found{};
    LaneCommand candidate{};

    for (std::size_t index = 0U; index < consumers_.size(); ++index) {
      if (!consumers_[index].try_peek(candidate)) {
        continue;
      }
      if (!found || less(candidate, selected_command_)) {
        selected = index;
        selected_command_ = candidate;
        found = true;
      }
    }

    if (!found || !consumers_[selected].try_pop(command)) {
      return false;
    }
    return true;
  }

private:
  [[nodiscard]] static bool less(const LaneCommand& left, const LaneCommand& right) noexcept {
    if (left.logical_time != right.logical_time) {
      return left.logical_time < right.logical_time;
    }
    if (left.lane_id != right.lane_id) {
      return left.lane_id < right.lane_id;
    }
    return left.lane_sequence < right.lane_sequence;
  }

  std::vector<LaneQueue::Consumer> consumers_;
  LaneCommand selected_command_{};
};

} // namespace matching_engine

#endif