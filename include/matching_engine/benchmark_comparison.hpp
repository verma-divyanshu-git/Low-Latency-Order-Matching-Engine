#ifndef MATCHING_ENGINE_BENCHMARK_COMPARISON_HPP
#define MATCHING_ENGINE_BENCHMARK_COMPARISON_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace matching_engine::benchmark_comparison {

inline constexpr std::uint64_t kMaximumComparisonOperations = 1'000'000U;

enum class Implementation : std::uint8_t {
  ladder_bitmap,
  standard_map,
  sorted_vector,
  abseil_btree,
};

struct Config {
  std::uint64_t active_levels{4'096U};
  std::uint64_t operations{100'000U};
  std::uint64_t repetitions{7U};
  std::optional<std::filesystem::path> output;
};

struct Result {
  Implementation implementation{Implementation::ladder_bitmap};
  std::vector<std::uint64_t> elapsed_ns;
  std::uint64_t median_elapsed_ns{};
  double median_operations_per_second{};
  std::uint64_t checksum{};
};

struct Report {
  Config config;
  std::vector<Result> results;
  bool validation_passed{};
};

[[nodiscard]] std::optional<Config> parse_cli(const std::vector<std::string>& arguments,
                                              std::string* error = nullptr);
[[nodiscard]] std::optional<Report> run(const Config& config, std::string& error);
[[nodiscard]] std::optional<std::string> report_json(const Report& report);
[[nodiscard]] const char* implementation_name(Implementation implementation) noexcept;

} // namespace matching_engine::benchmark_comparison

#endif
