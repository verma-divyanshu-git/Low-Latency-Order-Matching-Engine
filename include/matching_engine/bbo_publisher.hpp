#ifndef MATCHING_ENGINE_BBO_PUBLISHER_HPP
#define MATCHING_ENGINE_BBO_PUBLISHER_HPP

#include "matching_engine/bbo_protocol.hpp"

#include <optional>

namespace matching_engine {

enum class BboPublishStatus : std::uint8_t {
  published,
  unchanged,
  snapshot_unavailable,
  output_error,
};

class BboPublisher {
public:
  [[nodiscard]] BboPublishStatus try_publish(const BboSnapshot& snapshot,
                                              std::span<std::byte> output) noexcept;

  [[nodiscard]] BboFrameError last_frame_error() const noexcept {
    return last_frame_error_;
  }

private:
  std::optional<BboState> last_published_{};
  BboFrameError last_frame_error_{BboFrameError{}};
};

} // namespace matching_engine

#endif