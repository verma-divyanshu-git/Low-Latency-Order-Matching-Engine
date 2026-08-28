#ifndef MATCHING_ENGINE_MBP_PUBLISHER_HPP
#define MATCHING_ENGINE_MBP_PUBLISHER_HPP

#include "matching_engine/market_data_protocol.hpp"
#include "matching_engine/sequenced_engine.hpp"

namespace matching_engine {

enum class MbpPublishStatus : std::uint8_t {
  published,
  output_error,
};

class MbpPublisher {
public:
  [[nodiscard]] MbpPublishStatus publish_best(const EngineEvent& event, const OrderBook& book,
                                               Side side, std::span<std::byte> output) noexcept;

  [[nodiscard]] MarketDataFrameError last_error() const noexcept {
    return last_error_;
  }

private:
  MarketDataFrameError last_error_{MarketDataFrameError::none};
};

} // namespace matching_engine

#endif
