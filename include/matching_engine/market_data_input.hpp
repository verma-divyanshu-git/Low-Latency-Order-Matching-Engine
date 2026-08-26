#ifndef MATCHING_ENGINE_MARKET_DATA_INPUT_HPP
#define MATCHING_ENGINE_MARKET_DATA_INPUT_HPP

#include "matching_engine/market_data_protocol.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>

namespace matching_engine {

enum class MarketDataInputError : std::uint8_t {
  open_failed,
  truncated_frame,
  malformed_frame,
  sequence_gap,
};

class MarketDataInputStream {
public:
  [[nodiscard]] static std::expected<MarketDataInputStream, MarketDataInputError>
  open(const std::filesystem::path& path);

  [[nodiscard]] std::expected<std::optional<MarketDataMessage>, MarketDataInputError>
  read_next() noexcept;

private:
  explicit MarketDataInputStream(std::ifstream input) noexcept : input_{std::move(input)} {}

  std::ifstream input_;
  std::uint64_t previous_sequence_{};
};

} // namespace matching_engine

#endif