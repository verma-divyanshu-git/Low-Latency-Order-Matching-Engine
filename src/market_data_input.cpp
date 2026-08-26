#include "matching_engine/market_data_input.hpp"

#include <array>

namespace matching_engine {

std::expected<MarketDataInputStream, MarketDataInputError>
MarketDataInputStream::open(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input.is_open()) {
    return std::unexpected{MarketDataInputError::open_failed};
  }
  return MarketDataInputStream{std::move(input)};
}

std::expected<std::optional<MarketDataMessage>, MarketDataInputError>
MarketDataInputStream::read_next() noexcept {
  std::array<std::byte, kEncodedMarketDataFrameSize> frame{};
  input_.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  const std::streamsize bytes_read = input_.gcount();
  if (bytes_read == 0 && input_.eof()) {
    return std::optional<MarketDataMessage>{};
  }
  if (bytes_read != static_cast<std::streamsize>(frame.size())) {
    return std::unexpected{MarketDataInputError::truncated_frame};
  }
  const auto message = decode_market_data_frame(frame);
  if (!message.has_value()) {
    return std::unexpected{MarketDataInputError::malformed_frame};
  }
  if (validate_market_data_sequence(previous_sequence_, *message) != MarketDataFrameError::none) {
    return std::unexpected{MarketDataInputError::sequence_gap};
  }
  previous_sequence_ = message->sequence;
  return std::optional<MarketDataMessage>{*message};
}

} // namespace matching_engine