#ifndef MATCHING_ENGINE_RUNTIME_CONTROLS_HPP
#define MATCHING_ENGINE_RUNTIME_CONTROLS_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace matching_engine {

enum class RuntimeControlError : std::uint8_t {
  none,
  invalid_argument,
  unsupported,
  system_error,
};

[[nodiscard]] RuntimeControlError
pin_current_thread(std::span<const std::uint32_t> cpu_ids) noexcept;

[[nodiscard]] RuntimeControlError prepare_memory(std::span<std::byte> storage,
                                                  bool lock_pages) noexcept;

} // namespace matching_engine

#endif