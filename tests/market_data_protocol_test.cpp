#include "matching_engine/market_data_protocol.hpp"

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(MarketDataProtocolTest, EncodesAddOrderAsVersionedLittleEndianFrame) {
  const MarketDataMessage message{.sequence = 0x0102030405060708ULL,
                                  .order_id = OrderId{0x1112131415161718ULL},
                                  .price = Price{-9},
                                  .quantity = Quantity{0x2122232425262728ULL},
                                  .type = MarketDataMessageType::add_order,
                                  .side = Side::sell};
  std::array<std::byte, kEncodedMarketDataFrameSize> encoded{};

  ASSERT_EQ(encode_market_data_frame(message, encoded), MarketDataFrameError::none);
  EXPECT_EQ(encoded[0], std::byte{1});
  EXPECT_EQ(encoded[1], std::byte{1});
  EXPECT_EQ(encoded[2], std::byte{1});
  EXPECT_EQ(encoded[8], std::byte{0x08});
  EXPECT_EQ(encoded[16], std::byte{0x18});
  EXPECT_EQ(encoded[32], std::byte{0xf7});
  EXPECT_EQ(encoded[40], std::byte{0x28});
  EXPECT_EQ(decode_market_data_frame(encoded), message);
}

TEST(MarketDataProtocolTest, RoundTripsEveryCanonicalMessageType) {
  const std::array<MarketDataMessage, 5U> messages{
      MarketDataMessage{.sequence = 1U,
                        .order_id = OrderId{11U},
                        .price = Price{101},
                        .quantity = Quantity{12U},
                        .type = MarketDataMessageType::add_order,
                        .side = Side::buy},
      MarketDataMessage{.sequence = 2U,
                        .order_id = OrderId{11U},
                        .quantity = Quantity{7U},
                        .type = MarketDataMessageType::replace_order},
      MarketDataMessage{.sequence = 3U,
                        .order_id = OrderId{11U},
                        .type = MarketDataMessageType::delete_order},
      MarketDataMessage{.sequence = 4U,
                        .order_id = OrderId{12U},
                        .secondary_order_id = OrderId{13U},
                        .price = Price{102},
                        .quantity = Quantity{4U},
                        .type = MarketDataMessageType::trade},
      MarketDataMessage{.sequence = 5U,
                        .price = Price{103},
                        .quantity = Quantity{15U},
                        .order_count = 2U,
                        .type = MarketDataMessageType::level_update,
                        .side = Side::sell}};

  for (const MarketDataMessage& message : messages) {
    std::array<std::byte, kEncodedMarketDataFrameSize> encoded{};
    ASSERT_EQ(encode_market_data_frame(message, encoded), MarketDataFrameError::none);
    EXPECT_EQ(decode_market_data_frame(encoded), message);
  }
}

TEST(MarketDataProtocolTest, RejectsMalformedFramesAndDetectsSequenceGaps) {
  const MarketDataMessage message{.sequence = 4U,
                                  .order_id = OrderId{11U},
                                  .type = MarketDataMessageType::delete_order};
  std::array<std::byte, kEncodedMarketDataFrameSize> encoded{};
  ASSERT_EQ(encode_market_data_frame(message, encoded), MarketDataFrameError::none);

  EXPECT_EQ(decode_market_data_frame(std::span<const std::byte>{encoded.data(), 1U}).error(),
            MarketDataFrameError::invalid_length);
  encoded[0] = std::byte{2};
  EXPECT_EQ(decode_market_data_frame(encoded).error(), MarketDataFrameError::unsupported_version);
  encoded[0] = std::byte{1};
  encoded[1] = std::byte{99};
  EXPECT_EQ(decode_market_data_frame(encoded).error(), MarketDataFrameError::invalid_type);
  encoded[1] = std::byte{3};
  encoded[3] = std::byte{1};
  EXPECT_EQ(decode_market_data_frame(encoded).error(), MarketDataFrameError::noncanonical);
  encoded[3] = std::byte{};
  encoded[40] = std::byte{1};
  EXPECT_EQ(decode_market_data_frame(encoded).error(), MarketDataFrameError::noncanonical);

  EXPECT_EQ(validate_market_data_sequence(3U, message), MarketDataFrameError::none);
  EXPECT_EQ(validate_market_data_sequence(2U, message), MarketDataFrameError::sequence_gap);
  EXPECT_EQ(validate_market_data_sequence(UINT64_MAX, message), MarketDataFrameError::sequence_gap);
}

} // namespace matching_engine