#include "matching_engine/runtime_controls.hpp"

#include <cstdint>

#if defined(__linux__)
#include <cerrno>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace matching_engine {

RuntimeControlError pin_current_thread(std::span<const std::uint32_t> cpu_ids) noexcept {
  if (cpu_ids.empty()) {
    return RuntimeControlError::invalid_argument;
  }

#if defined(__linux__)
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  for (const std::uint32_t cpu_id : cpu_ids) {
    if (cpu_id >= CPU_SETSIZE) {
      return RuntimeControlError::invalid_argument;
    }
    CPU_SET(cpu_id, &cpu_set);
  }
  return sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == 0 ? RuntimeControlError::none
                                                               : RuntimeControlError::system_error;
#else
  return RuntimeControlError::unsupported;
#endif
}

RuntimeControlError prepare_memory(std::span<std::byte> storage, bool lock_pages) noexcept {
  if (storage.empty()) {
    return RuntimeControlError::invalid_argument;
  }

#if defined(__linux__)
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0L) {
    return RuntimeControlError::system_error;
  }
  for (std::size_t offset = 0U; offset < storage.size(); offset += static_cast<std::size_t>(page_size)) {
    storage[offset] = std::byte{};
  }
  storage.back() = std::byte{};
  if (lock_pages && mlock(storage.data(), storage.size()) != 0) {
    return RuntimeControlError::system_error;
  }
  return RuntimeControlError::none;
#else
  (void)lock_pages;
  for (std::size_t offset = 0U; offset < storage.size(); offset += 4096U) {
    storage[offset] = std::byte{};
  }
  storage.back() = std::byte{};
  return RuntimeControlError::unsupported;
#endif
}

} // namespace matching_engine