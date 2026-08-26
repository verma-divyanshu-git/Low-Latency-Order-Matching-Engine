#include "matching_engine/command_protocol.hpp"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>

namespace matching_engine {

TEST(CommandProtocolTest, EncodesAndDecodesVersionedLittleEndianFrame) {
  const CommandPayload payload =
      CommandPayload::submit_limit(OrderId{0x0102030405060708ULL}, Side::sell, Price{-9},
                                   Quantity{0x1112131415161718ULL}, TimeInForce::ioc);
  std::array<std::byte, kEncodedCommandFrameSize> encoded{};

  ASSERT_EQ(encode_command_frame(payload, encoded), CommandFrameError{});
  EXPECT_EQ(encoded[0], std::byte{1});
  EXPECT_EQ(encoded[1], std::byte{});
  EXPECT_EQ(encoded[4], std::byte{1});
  EXPECT_EQ(encoded[5], std::byte{1});
  EXPECT_EQ(decode_command_frame(encoded), payload);
}

TEST(CommandProtocolTest, RejectsBadLengthVersionHeaderAndPayload) {
  const CommandPayload payload = CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{2U});
  std::array<std::byte, kEncodedCommandFrameSize> encoded{};
  ASSERT_EQ(encode_command_frame(payload, encoded), CommandFrameError{});

  EXPECT_EQ(decode_command_frame(std::span<const std::byte>{encoded.data(), encoded.size() - 1U})
                .error(),
            CommandFrameError::invalid_length);
  encoded[0] = std::byte{2};
  EXPECT_EQ(decode_command_frame(encoded).error(), CommandFrameError::unsupported_version);
  encoded[0] = std::byte{1};
  encoded[2] = std::byte{1};
  EXPECT_EQ(decode_command_frame(encoded).error(), CommandFrameError::noncanonical_header);
}

} // namespace matching_engine