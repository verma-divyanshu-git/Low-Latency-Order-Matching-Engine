#include "matching_engine/bbo_publisher.hpp"

namespace matching_engine {

BboPublishStatus BboPublisher::try_publish(const BboSnapshot& snapshot,
                                            std::span<std::byte> output) noexcept {
  const auto state = snapshot.read();
  if (!state.has_value()) {
    return BboPublishStatus::snapshot_unavailable;
  }
  if (last_published_ == state) {
    return BboPublishStatus::unchanged;
  }
  last_frame_error_ = encode_bbo_frame(*state, output);
  if (last_frame_error_ != BboFrameError{}) {
    return BboPublishStatus::output_error;
  }
  last_published_ = *state;
  return BboPublishStatus::published;
}

} // namespace matching_engine