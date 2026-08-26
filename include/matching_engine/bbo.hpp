#ifndef MATCHING_ENGINE_BBO_HPP
#define MATCHING_ENGINE_BBO_HPP

#include "matching_engine/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace matching_engine {

struct BboState {
  std::optional<Price> bid_price;
  Quantity bid_quantity{0U};
  std::optional<Price> ask_price;
  Quantity ask_quantity{0U};

  constexpr bool operator==(const BboState&) const noexcept = default;
};

class BboSnapshot {
public:
  static constexpr std::size_t kDefaultReaderRetries = 8U;

  explicit BboSnapshot(std::size_t max_reader_retries = kDefaultReaderRetries) noexcept
      : max_reader_retries_{max_reader_retries} {}

  void publish(const BboState& state) noexcept;

  [[nodiscard]] std::optional<BboState> read() const noexcept;

private:
  std::atomic<std::uint64_t> version_{0U};
  std::atomic<std::int64_t> bid_price_{0};
  std::atomic<std::uint64_t> bid_quantity_{0U};
  std::atomic<std::int64_t> ask_price_{0};
  std::atomic<std::uint64_t> ask_quantity_{0U};
  std::atomic<std::uint8_t> bid_present_{0U};
  std::atomic<std::uint8_t> ask_present_{0U};
  std::size_t max_reader_retries_;
};

} // namespace matching_engine

#endif