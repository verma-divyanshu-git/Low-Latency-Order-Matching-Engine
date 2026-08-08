#ifndef MATCHING_ENGINE_COMMAND_HPP
#define MATCHING_ENGINE_COMMAND_HPP

#include "matching_engine/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace matching_engine {

enum class CommandType : std::uint8_t {
  submit_limit = 1U,
  submit_market = 2U,
  cancel = 3U,
  amend_quantity = 4U,
  replace = 5U,
};

enum class CommandValidationError : std::uint8_t {
  none,
  invalid_tag,
  invalid_side,
  invalid_time_in_force,
  noncanonical,
};

enum class CommandCodecError : std::uint8_t {
  none,
  invalid_length,
  invalid_tag,
  invalid_side,
  invalid_time_in_force,
  noncanonical,
};

struct CommandPayload {
  CommandType tag{CommandType::submit_limit};
  Side side{Side::buy};
  TimeInForce time_in_force{TimeInForce::gtc};
  std::uint8_t reserved{};
  std::uint64_t order_id{};
  std::int64_t price_ticks{};
  std::uint64_t quantity{};
  std::uint32_t handle_index{};
  std::uint32_t handle_generation{};

  [[nodiscard]] static constexpr CommandPayload
  submit_limit(OrderId id, Side order_side, Price price, Quantity order_quantity,
               TimeInForce tif = TimeInForce::gtc) noexcept {
    return {.tag = CommandType::submit_limit,
            .side = order_side,
            .time_in_force = tif,
            .order_id = id.value(),
            .price_ticks = price.ticks(),
            .quantity = order_quantity.value()};
  }

  [[nodiscard]] static constexpr CommandPayload submit_market(OrderId id, Side order_side,
                                                              Quantity order_quantity) noexcept {
    return {.tag = CommandType::submit_market,
            .side = order_side,
            .order_id = id.value(),
            .quantity = order_quantity.value()};
  }

  [[nodiscard]] static constexpr CommandPayload cancel(Handle handle) noexcept {
    return {.tag = CommandType::cancel,
            .handle_index = handle.index,
            .handle_generation = handle.generation};
  }

  [[nodiscard]] static constexpr CommandPayload amend_quantity(Handle handle,
                                                               Quantity new_quantity) noexcept {
    return {.tag = CommandType::amend_quantity,
            .quantity = new_quantity.value(),
            .handle_index = handle.index,
            .handle_generation = handle.generation};
  }

  [[nodiscard]] static constexpr CommandPayload replace(Handle handle, Price new_price,
                                                        Quantity new_quantity) noexcept {
    return {.tag = CommandType::replace,
            .price_ticks = new_price.ticks(),
            .quantity = new_quantity.value(),
            .handle_index = handle.index,
            .handle_generation = handle.generation};
  }

  constexpr bool operator==(const CommandPayload&) const noexcept = default;
};

inline constexpr std::size_t kEncodedCommandPayloadSize = 36U;

[[nodiscard]] constexpr bool is_valid_time_in_force(TimeInForce value) noexcept {
  return value == TimeInForce::gtc || value == TimeInForce::ioc || value == TimeInForce::fok;
}

[[nodiscard]] CommandValidationError
validate_command_payload(const CommandPayload& payload) noexcept;
[[nodiscard]] CommandCodecError encode_command_payload(const CommandPayload& payload,
                                                       std::span<std::byte> output) noexcept;
[[nodiscard]] std::expected<CommandPayload, CommandCodecError>
decode_command_payload(std::span<const std::byte> input) noexcept;

struct SequencedCommand {
  CommandPayload payload{};
  Sequence sequence{0U};
  std::uint64_t logical_time{};

  constexpr bool operator==(const SequencedCommand&) const noexcept = default;
};

enum class SequencerError : std::uint8_t {
  invalid_payload,
  decreasing_logical_time,
  sequence_exhausted,
};

class Sequencer {
public:
  constexpr Sequencer() noexcept = default;
  constexpr explicit Sequencer(Sequence next_sequence, std::uint64_t last_logical_time) noexcept
      : next_sequence_{next_sequence}, last_logical_time_{last_logical_time} {}

  [[nodiscard]] std::expected<SequencedCommand, SequencerError>
  stamp(const CommandPayload& payload, std::uint64_t logical_time) noexcept;

  [[nodiscard]] constexpr Sequence next_sequence() const noexcept {
    return next_sequence_;
  }
  [[nodiscard]] constexpr std::uint64_t last_logical_time() const noexcept {
    return last_logical_time_;
  }
  [[nodiscard]] constexpr bool exhausted() const noexcept {
    return exhausted_;
  }

private:
  Sequence next_sequence_{1U};
  std::uint64_t last_logical_time_{};
  bool exhausted_{};
};

static_assert(std::is_trivially_copyable_v<CommandPayload>);
static_assert(std::is_trivially_copyable_v<SequencedCommand>);

} // namespace matching_engine

#endif
