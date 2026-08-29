#ifndef MATCHING_ENGINE_RUNTIME_OPERATIONS_HPP
#define MATCHING_ENGINE_RUNTIME_OPERATIONS_HPP

#include "matching_engine/pipeline.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace matching_engine {

enum class RuntimeLifecycle : std::uint8_t {
  starting,
  running,
  draining,
  stopped,
  failed,
};

enum class RuntimeHealth : std::uint8_t {
  healthy,
  backpressured,
  capacity_exhausted,
  recovery_required,
  corruption,
};

struct RuntimeHealthSnapshot {
  RuntimeLifecycle lifecycle{RuntimeLifecycle::starting};
  RuntimeHealth health{RuntimeHealth::healthy};
  std::uint64_t accepted_commands{};
  std::uint64_t matched_commands{};
  std::uint64_t published_events{};
  std::uint64_t backpressure_events{};
  std::uint64_t capacity_events{};
  std::uint64_t failure_events{};

  constexpr bool operator==(const RuntimeHealthSnapshot&) const noexcept = default;
};

class RuntimeOperations {
public:
  [[nodiscard]] bool start() noexcept;
  [[nodiscard]] bool begin_shutdown() noexcept;
  [[nodiscard]] bool complete_shutdown(bool command_queue_empty,
                                       bool event_queue_empty) noexcept;

  void observe(IngressStatus status) noexcept;
  void observe(MatchingStatus status) noexcept;
  void observe(PublicationStatus status) noexcept;

  [[nodiscard]] bool accepts_ingress() const noexcept {
    return snapshot_.lifecycle == RuntimeLifecycle::running &&
           snapshot_.health != RuntimeHealth::recovery_required &&
           snapshot_.health != RuntimeHealth::corruption;
  }
  [[nodiscard]] RuntimeHealthSnapshot snapshot() const noexcept {
    return snapshot_;
  }

private:
  void raise_health(RuntimeHealth health) noexcept;
  void fail(RuntimeHealth health) noexcept;

  RuntimeHealthSnapshot snapshot_{};
};

[[nodiscard]] const char* runtime_lifecycle_name(RuntimeLifecycle lifecycle) noexcept;
[[nodiscard]] const char* runtime_health_name(RuntimeHealth health) noexcept;
[[nodiscard]] std::optional<std::string>
runtime_health_json(const RuntimeHealthSnapshot& snapshot);

} // namespace matching_engine

#endif
