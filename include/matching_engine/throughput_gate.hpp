#ifndef MATCHING_ENGINE_THROUGHPUT_GATE_HPP
#define MATCHING_ENGINE_THROUGHPUT_GATE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace matching_engine::throughput_gate {

inline constexpr std::uint64_t kMaximumSamples = 1'000'000U;
inline constexpr std::uint64_t kMinimumRepetitions = 3U;
inline constexpr std::uint64_t kMaximumRepetitions = 21U;

struct Config {
  std::uint64_t samples{100'000U};
  std::uint64_t repetitions{7U};
  double minimum_ops_per_second{1'000'000.0};
  double maximum_relative_mad{0.25};
  std::optional<std::filesystem::path> output;
};

struct Statistics {
  std::uint64_t samples{};
  std::uint64_t repetitions{};
  std::vector<std::uint64_t> elapsed_ns;
  std::uint64_t best_elapsed_ns{};
  std::uint64_t median_elapsed_ns{};
  std::uint64_t median_absolute_deviation_ns{};
  double relative_mad{};
  double best_ops_per_second{};
  bool validation_passed{};
  std::uint64_t checksum{};
  bool gate_passed{};
};

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values) noexcept;
[[nodiscard]] std::optional<Statistics> summarize_durations(std::vector<std::uint64_t> durations,
                                                            std::uint64_t samples);
[[nodiscard]] bool passes_gate(const Statistics& statistics, double minimum_ops_per_second,
                               double maximum_relative_mad) noexcept;
[[nodiscard]] std::optional<std::string> statistics_json(const Statistics& statistics,
                                                         double minimum_ops_per_second,
                                                         double maximum_relative_mad);
[[nodiscard]] std::optional<Config> parse_cli(const std::vector<std::string>& arguments,
                                              std::string* error = nullptr);
[[nodiscard]] std::optional<Statistics> run(const Config& config, std::string& error);

} // namespace matching_engine::throughput_gate

#endif
