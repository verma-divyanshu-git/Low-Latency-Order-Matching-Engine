#include "matching_engine/mbo_publisher.hpp"

namespace matching_engine {

MboPublishStatus MboPublisher::publish(const EngineEvent& event, std::span<std::byte> output) noexcept {
  last_error_ = encode_engine_event(event, output);
  return last_error_ == EventCodecError::none ? MboPublishStatus::published
                                               : MboPublishStatus::output_error;
}

} // namespace matching_engine