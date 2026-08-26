#include "matching_engine/bbo_protocol.hpp"

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(BboProtocolTest, EncodesAndDecodesVersionedLittleEndianFrame) {
  const BboState state{.bid_price = Price{-9},
                       .bid_quantity = Quantity{0x1112131415161718ULL},
                       .ask_price = Price{7},
                       .ask_quantity = Quantity{0x2122232425262728ULL}};
  std::array<std::byte, kEncodedBboFrameSize> encoded{};

  ASSERT_EQ(encode_bbo_frame(state, encoded), BboFrameError{});
  EXPECT_EQ(encoded[0], std::byte{1});
  EXPECT_EQ(encoded[4], std::byte{1});
  EXPECT_EQ(encoded[5], std::byte{1});
  EXPECT_EQ(encoded[8], std::byte{0xf7});
  EXPECT_EQ(encoded[16], std::byte{0x18});
  EXPECT_EQ(decode_bbo_frame(encoded), state);
}

TEST(BboProtocolTest, CanonicalizesMissingSidesAndRejectsMalformedFrames) {
  const BboState state{.bid_price = std::nullopt,
                       .bid_quantity = Quantity{99U},
                       .ask_price = Price{7},
                       .ask_quantity = Quantity{3U}};
  std::array<std::byte, kEncodedBboFrameSize> encoded{};

  ASSERT_EQ(encode_bbo_frame(state, encoded), BboFrameError{});
  EXPECT_EQ(encoded[4], std::byte{});
  EXPECT_EQ(encoded[8], std::byte{});
  EXPECT_EQ(encoded[16], std::byte{});
  const BboState expected{.bid_price = std::nullopt,
                          .bid_quantity = Quantity{0U},
                          .ask_price = Price{7},
                          .ask_quantity = Quantity{3U}};
  EXPECT_EQ(decode_bbo_frame(encoded), expected);

  encoded[0] = std::byte{2};
  EXPECT_EQ(decode_bbo_frame(encoded).error(), BboFrameError::unsupported_version);
  encoded[0] = std::byte{1};
  encoded[4] = std::byte{2};
  EXPECT_EQ(decode_bbo_frame(encoded).error(), BboFrameError::noncanonical);
  encoded[4] = std::byte{};
  encoded[8] = std::byte{1};
  EXPECT_EQ(decode_bbo_frame(encoded).error(), BboFrameError::noncanonical);
}

} // namespace matching_engine