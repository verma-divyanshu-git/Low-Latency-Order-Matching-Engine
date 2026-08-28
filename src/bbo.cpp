#include "matching_engine/bbo.hpp"

namespace matching_engine {

void BboSnapshot::publish(const BboState& state) noexcept {
  version_.fetch_add(1U, std::memory_order_seq_cst);
  bid_price_.store(state.bid_price.value_or(Price{0}).ticks(), std::memory_order_seq_cst);
  bid_quantity_.store(state.bid_quantity.value(), std::memory_order_seq_cst);
  ask_price_.store(state.ask_price.value_or(Price{0}).ticks(), std::memory_order_seq_cst);
  ask_quantity_.store(state.ask_quantity.value(), std::memory_order_seq_cst);
  bid_present_.store(state.bid_price.has_value() ? 1U : 0U, std::memory_order_seq_cst);
  ask_present_.store(state.ask_price.has_value() ? 1U : 0U, std::memory_order_seq_cst);
  version_.fetch_add(1U, std::memory_order_seq_cst);
}

std::optional<BboState> BboSnapshot::read() const noexcept {
  for (std::size_t attempt = 0U; attempt < max_reader_retries_; ++attempt) {
    const std::uint64_t before = version_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0U) {
      continue;
    }

    const BboState state{
        .bid_price = bid_present_.load(std::memory_order_seq_cst) != 0U
                 ? std::optional<Price>{Price{bid_price_.load(std::memory_order_seq_cst)}}
                         : std::nullopt,
        .bid_quantity = Quantity{bid_quantity_.load(std::memory_order_seq_cst)},
        .ask_price = ask_present_.load(std::memory_order_seq_cst) != 0U
                 ? std::optional<Price>{Price{ask_price_.load(std::memory_order_seq_cst)}}
                         : std::nullopt,
        .ask_quantity = Quantity{ask_quantity_.load(std::memory_order_seq_cst)}};
      const std::uint64_t after = version_.load(std::memory_order_seq_cst);
    if (before == after) {
      return state;
    }
  }
  return std::nullopt;
}

} // namespace matching_engine