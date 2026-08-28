#include "matching_engine/mbp_publisher.hpp"

namespace matching_engine {

MbpPublishStatus MbpPublisher::publish_best(const EngineEvent& event, const OrderBook& book, Side side,
                                            std::span<std::byte> output) noexcept {
  const std::optional<Price> price = side == Side::buy ? book.best_bid() : book.best_ask();
  const std::optional<LevelInfo> level = price.has_value() ? book.level_info(side, *price) : std::nullopt;
  const MarketDataMessage message{.sequence = event.command_sequence.value(),
                                  .price = price.value_or(Price{0}),
                                  .quantity = level.has_value() ? level->aggregate_quantity : Quantity{0U},
                                  .order_count = level.has_value() ? level->order_count : 0U,
                                  .type = MarketDataMessageType::level_update,
                                  .side = side};
  last_error_ = encode_market_data_frame(message, output);
  return last_error_ == MarketDataFrameError::none ? MbpPublishStatus::published
                                                    : MbpPublishStatus::output_error;
}

} // namespace matching_engine
