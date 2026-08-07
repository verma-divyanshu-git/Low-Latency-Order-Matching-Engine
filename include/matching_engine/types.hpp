#ifndef MATCHING_ENGINE_TYPES_HPP
#define MATCHING_ENGINE_TYPES_HPP

#include <compare>
#include <cstdint>

namespace matching_engine {

class Price {
public:
  explicit constexpr Price(std::int64_t ticks) noexcept : ticks_{ticks} {}

  [[nodiscard]] constexpr std::int64_t ticks() const noexcept {
    return ticks_;
  }

  constexpr auto operator<=>(const Price&) const noexcept = default;

private:
  std::int64_t ticks_;
};

class Quantity {
public:
  explicit constexpr Quantity(std::uint64_t value) noexcept : value_{value} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  constexpr auto operator<=>(const Quantity&) const noexcept = default;

private:
  std::uint64_t value_;
};

class OrderId {
public:
  explicit constexpr OrderId(std::uint64_t value) noexcept : value_{value} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  constexpr auto operator<=>(const OrderId&) const noexcept = default;

private:
  std::uint64_t value_;
};

class Sequence {
public:
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_{value} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  constexpr auto operator<=>(const Sequence&) const noexcept = default;

private:
  std::uint64_t value_;
};

enum class Side : std::uint8_t {
  buy,
  sell,
};

[[nodiscard]] constexpr bool is_valid_side(Side side) noexcept {
  return side == Side::buy || side == Side::sell;
}

struct Handle {
  std::uint32_t index{};
  std::uint32_t generation{};

  constexpr bool operator==(const Handle&) const noexcept = default;
};

} // namespace matching_engine

#endif
