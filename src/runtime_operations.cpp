#include "matching_engine/runtime_operations.hpp"

#include <limits>
#include <sstream>

namespace matching_engine {
namespace {

void increment(std::uint64_t& value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

} // namespace

bool RuntimeOperations::start() noexcept {
  if (snapshot_.lifecycle != RuntimeLifecycle::starting) {
    return false;
  }
  snapshot_.lifecycle = RuntimeLifecycle::running;
  return true;
}

bool RuntimeOperations::begin_shutdown() noexcept {
  if (snapshot_.lifecycle != RuntimeLifecycle::running) {
    return false;
  }
  snapshot_.lifecycle = RuntimeLifecycle::draining;
  return true;
}

bool RuntimeOperations::complete_shutdown(bool command_queue_empty,
                                          bool event_queue_empty) noexcept {
  if (snapshot_.lifecycle != RuntimeLifecycle::draining || !command_queue_empty ||
      !event_queue_empty) {
    return false;
  }
  snapshot_.lifecycle = RuntimeLifecycle::stopped;
  return true;
}

void RuntimeOperations::raise_health(RuntimeHealth health) noexcept {
  if (static_cast<std::uint8_t>(health) > static_cast<std::uint8_t>(snapshot_.health)) {
    snapshot_.health = health;
  }
}

void RuntimeOperations::fail(RuntimeHealth health) noexcept {
  raise_health(health);
  increment(snapshot_.failure_events);
  snapshot_.lifecycle = RuntimeLifecycle::failed;
}

void RuntimeOperations::observe(IngressStatus status) noexcept {
  switch (status) {
  case IngressStatus::progress:
    increment(snapshot_.accepted_commands);
    return;
  case IngressStatus::queue_backpressure:
    raise_health(RuntimeHealth::backpressured);
    increment(snapshot_.backpressure_events);
    return;
  case IngressStatus::journal_full:
  case IngressStatus::sequence_exhausted:
    raise_health(RuntimeHealth::capacity_exhausted);
    increment(snapshot_.capacity_events);
    return;
  case IngressStatus::persistence_failure:
  case IngressStatus::commit_indeterminate:
  case IngressStatus::recovery_required:
    fail(RuntimeHealth::recovery_required);
    return;
  case IngressStatus::invalid_payload:
  case IngressStatus::decreasing_logical_time:
    increment(snapshot_.failure_events);
    return;
  case IngressStatus::stopped_poisoned:
    fail(RuntimeHealth::corruption);
    return;
  }
}

void RuntimeOperations::observe(MatchingStatus status) noexcept {
  switch (status) {
  case MatchingStatus::progress:
    increment(snapshot_.matched_commands);
    return;
  case MatchingStatus::input_empty:
    return;
  case MatchingStatus::output_backpressure:
    raise_health(RuntimeHealth::backpressured);
    increment(snapshot_.backpressure_events);
    return;
  case MatchingStatus::sequence_exhausted:
  case MatchingStatus::impossible_event_capacity:
    raise_health(RuntimeHealth::capacity_exhausted);
    increment(snapshot_.capacity_events);
    return;
  case MatchingStatus::invalid_command:
  case MatchingStatus::invalid_sequence:
  case MatchingStatus::decreasing_logical_time:
  case MatchingStatus::internal_invariant_failure:
  case MatchingStatus::poisoned:
    fail(RuntimeHealth::corruption);
    return;
  }
}

void RuntimeOperations::observe(PublicationStatus status) noexcept {
  if (status == PublicationStatus::progress) {
    increment(snapshot_.published_events);
  }
}

const char* runtime_lifecycle_name(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
  case RuntimeLifecycle::starting: return "starting";
  case RuntimeLifecycle::running: return "running";
  case RuntimeLifecycle::draining: return "draining";
  case RuntimeLifecycle::stopped: return "stopped";
  case RuntimeLifecycle::failed: return "failed";
  }
  return "unknown";
}

const char* runtime_health_name(RuntimeHealth health) noexcept {
  switch (health) {
  case RuntimeHealth::healthy: return "healthy";
  case RuntimeHealth::backpressured: return "backpressured";
  case RuntimeHealth::capacity_exhausted: return "capacity_exhausted";
  case RuntimeHealth::recovery_required: return "recovery_required";
  case RuntimeHealth::corruption: return "corruption";
  }
  return "unknown";
}

std::optional<std::string> runtime_health_json(const RuntimeHealthSnapshot& snapshot) {
  try {
    std::ostringstream output;
    output << "{\"schema_version\":1,\"lifecycle\":\""
           << runtime_lifecycle_name(snapshot.lifecycle) << "\",\"health\":\""
           << runtime_health_name(snapshot.health) << "\",\"accepted_commands\":"
           << snapshot.accepted_commands << ",\"matched_commands\":"
           << snapshot.matched_commands << ",\"published_events\":"
           << snapshot.published_events << ",\"backpressure_events\":"
           << snapshot.backpressure_events << ",\"capacity_events\":"
           << snapshot.capacity_events << ",\"failure_events\":"
           << snapshot.failure_events << '}';
    return output.str();
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace matching_engine
