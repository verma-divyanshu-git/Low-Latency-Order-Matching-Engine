#include "matching_engine/market_data_input.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

namespace matching_engine {
namespace {

class MarketDataInputFile {
public:
  MarketDataInputFile() {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("matching-engine-market-data-" + std::to_string(::getpid()) + "-" +
             std::to_string(++counter));
  }
  ~MarketDataInputFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  void write(std::span<const std::byte> bytes) const {
    std::ofstream output{path_, std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] MarketDataMessage delete_message(std::uint64_t sequence) {
  return {.sequence = sequence, .order_id = OrderId{sequence}, .type = MarketDataMessageType::delete_order};
}

} // namespace

TEST(MarketDataInputTest, ReadsValidatedFramesUntilEndOfFile) {
  MarketDataInputFile file;
  std::array<std::byte, kEncodedMarketDataFrameSize * 2U> bytes{};
  ASSERT_EQ(encode_market_data_frame(delete_message(1U),
                                     std::span<std::byte>{bytes.data(), kEncodedMarketDataFrameSize}),
            MarketDataFrameError::none);
  ASSERT_EQ(encode_market_data_frame(delete_message(2U),
                                     std::span<std::byte>{bytes.data() + kEncodedMarketDataFrameSize,
                                                          kEncodedMarketDataFrameSize}),
            MarketDataFrameError::none);
  file.write(bytes);

  auto input = MarketDataInputStream::open(file.path());
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ(input->read_next()->value(), delete_message(1U));
  EXPECT_EQ(input->read_next()->value(), delete_message(2U));
  EXPECT_FALSE(input->read_next()->has_value());
}

TEST(MarketDataInputTest, RejectsMalformedTruncatedAndDiscontinuousFrames) {
  MarketDataInputFile file;
  std::array<std::byte, kEncodedMarketDataFrameSize * 2U> bytes{};
  ASSERT_EQ(encode_market_data_frame(delete_message(1U),
                                     std::span<std::byte>{bytes.data(), kEncodedMarketDataFrameSize}),
            MarketDataFrameError::none);
  ASSERT_EQ(encode_market_data_frame(delete_message(3U),
                                     std::span<std::byte>{bytes.data() + kEncodedMarketDataFrameSize,
                                                          kEncodedMarketDataFrameSize}),
            MarketDataFrameError::none);
  file.write(bytes);
  auto input = MarketDataInputStream::open(file.path());
  ASSERT_TRUE(input.has_value());
  ASSERT_TRUE(input->read_next().has_value());
  EXPECT_EQ(input->read_next().error(), MarketDataInputError::sequence_gap);

  bytes[0] = std::byte{2};
  file.write(std::span<const std::byte>{bytes.data(), kEncodedMarketDataFrameSize});
  input = MarketDataInputStream::open(file.path());
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ(input->read_next().error(), MarketDataInputError::malformed_frame);

  file.write(std::span<const std::byte>{bytes.data(), 1U});
  input = MarketDataInputStream::open(file.path());
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ(input->read_next().error(), MarketDataInputError::truncated_frame);
}

} // namespace matching_engine