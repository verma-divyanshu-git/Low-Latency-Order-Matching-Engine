#ifndef MATCHING_ENGINE_MBO_PUBLISHER_HPP
#define MATCHING_ENGINE_MBO_PUBLISHER_HPP

#include "matching_engine/replay.hpp"

namespace matching_engine {

enum class MboPublishStatus : std::uint8_t {
  published,
  output_error,
};

class MboPublisher {
public:
  [[nodiscard]] MboPublishStatus publish(const EngineEvent& event,
                                         std::span<std::byte> output) noexcept;

  [[nodiscard]] EventCodecError last_error() const noexcept {
    return last_error_;
  }

private:
  EventCodecError last_error_{EventCodecError::none};
};

} // namespace matching_engine

#endif