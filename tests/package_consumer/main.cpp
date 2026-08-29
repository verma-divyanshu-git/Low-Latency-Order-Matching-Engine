#include "matching_engine/runtime_config.hpp"
#include "matching_engine/runtime_operations.hpp"

int main() {
  static_assert(matching_engine::kRuntimeApiVersionMajor == 1U);
  matching_engine::RuntimeOperations operations;
  return operations.start() && matching_engine::runtime_api_compatible(1U, 0U) ? 0 : 1;
}
