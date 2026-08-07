#ifndef MATCHING_ENGINE_PRICE_DOMAIN_HPP
#define MATCHING_ENGINE_PRICE_DOMAIN_HPP

#include "matching_engine/types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace matching_engine {

class PriceDomain {
public:
  PriceDomain(Price minimum, std::uint32_t tick_count)
      : minimum_{minimum}, maximum_{checked_maximum(minimum, tick_count)}, tick_count_{tick_count} {
  }

  [[nodiscard]] bool contains(Price price) const noexcept {
    return price >= minimum_ && price <= maximum_;
  }

  [[nodiscard]] std::optional<std::uint32_t> index_of(Price price) const noexcept {
    if (!contains(price)) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(price.ticks() - minimum_.ticks());
  }

  [[nodiscard]] std::optional<Price> price_at(std::uint32_t index) const noexcept {
    if (index >= tick_count_) {
      return std::nullopt;
    }
    return Price{minimum_.ticks() + static_cast<std::int64_t>(index)};
  }

private:
  [[nodiscard]] static Price checked_maximum(Price minimum, std::uint32_t tick_count) {
    if (tick_count == 0U) {
      throw std::invalid_argument{"PriceDomain tick count must be positive"};
    }

    const auto offset = static_cast<std::int64_t>(tick_count - 1U);
    if (minimum.ticks() > std::numeric_limits<std::int64_t>::max() - offset) {
      throw std::overflow_error{"PriceDomain maximum price exceeds int64 ticks"};
    }
    return Price{minimum.ticks() + offset};
  }

  Price minimum_;
  Price maximum_;
  std::uint32_t tick_count_;
};

} // namespace matching_engine

#endif
