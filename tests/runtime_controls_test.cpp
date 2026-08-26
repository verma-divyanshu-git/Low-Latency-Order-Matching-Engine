#include "matching_engine/runtime_controls.hpp"

#include <array>
#include <cstddef>
#include <gtest/gtest.h>

namespace matching_engine {

TEST(RuntimeControlsTest, RejectsEmptyCpuSet) {
  EXPECT_EQ(pin_current_thread({}), RuntimeControlError::invalid_argument);
}

TEST(RuntimeControlsTest, ReportsPlatformAffinityCapability) {
  const std::array<std::uint32_t, 1U> cpu_ids{0U};
  const RuntimeControlError result = pin_current_thread(cpu_ids);
#if defined(__linux__)
  EXPECT_TRUE(result == RuntimeControlError::none || result == RuntimeControlError::system_error);
#else
  EXPECT_EQ(result, RuntimeControlError::unsupported);
#endif
}

TEST(RuntimeControlsTest, PrefaultsProvidedStorage) {
  std::array<std::byte, 4096U> storage{};
#if defined(__linux__)
  EXPECT_EQ(prepare_memory(storage, false), RuntimeControlError::none);
#else
  EXPECT_EQ(prepare_memory(storage, false), RuntimeControlError::unsupported);
#endif
}

TEST(RuntimeControlsTest, RejectsEmptyStorage) {
  EXPECT_EQ(prepare_memory({}, false), RuntimeControlError::invalid_argument);
}

} // namespace matching_engine