#ifndef MATCHING_ENGINE_DETAIL_INVARIANT_HPP
#define MATCHING_ENGINE_DETAIL_INVARIANT_HPP

#include <exception>

namespace matching_engine::detail {

[[noreturn]] inline void invariant_failure() noexcept {
  std::terminate();
}

} // namespace matching_engine::detail

#endif
