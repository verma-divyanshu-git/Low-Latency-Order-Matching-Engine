#pragma once

#include "matching_engine/measurement.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct hdr_histogram;

namespace matching_engine::benchmark {

inline constexpr std::uint64_t kMaximumSamples = 1'000'000U;
inline constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;

struct TickRatio {
  std::uint64_t numerator{};
  std::uint64_t denominator{};
};

[[nodiscard]] std::optional<std::vector<std::uint64_t>>
make_schedule(std::uint64_t samples, std::uint64_t rate, TickRatio ticks_per_nanosecond);

struct EventObservation {
  measurement::ElapsedStatus status{measurement::ElapsedStatus::valid};
  std::uint64_t lateness_ticks{};
  std::uint64_t service_ticks{};
  std::uint64_t latency_ticks{};
};

[[nodiscard]] EventObservation observe_event(measurement::ClockSample intended,
                                             measurement::ClockSample start,
                                             measurement::ClockSample completion,
                                             measurement::ClockCapabilities capabilities) noexcept;
[[nodiscard]] measurement::ClockSample
wait_until_intended(measurement::ClockReader clock, measurement::ClockSample intended,
                    measurement::ClockCapabilities capabilities) noexcept;

struct Bucket {
  std::uint64_t value{};
  std::uint64_t count{};
  constexpr bool operator==(const Bucket&) const noexcept = default;
};

class Histogram {
public:
  Histogram(std::uint64_t lowest, std::uint64_t highest, int significant_figures);
  ~Histogram() = default;
  Histogram(Histogram&&) noexcept;
  Histogram& operator=(Histogram&&) noexcept;
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool record(std::uint64_t value) noexcept;
  [[nodiscard]] bool record_corrected(std::uint64_t value,
                                      std::uint64_t expected_interval) noexcept;
  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] std::uint64_t minimum() const noexcept;
  [[nodiscard]] std::uint64_t maximum() const noexcept;
  [[nodiscard]] std::uint64_t percentile(double percentile) const noexcept;
  [[nodiscard]] double mean() const noexcept;
  [[nodiscard]] std::vector<Bucket> recorded_buckets() const;
  [[nodiscard]] hdr_histogram* native() noexcept;

private:
  struct Deleter {
    void operator()(hdr_histogram* histogram) const noexcept;
  };
  std::unique_ptr<hdr_histogram, Deleter> histogram_;
};

enum class Mode : std::uint8_t {
  open_loop,
  closed_loop_diagnostic,
};

enum class Scenario : std::uint8_t {
  crossing_limit,
  sweep_3_level,
  none,
};

enum class ClaimScope : std::uint8_t {
  regression_only,
  publishable_candidate,
};

struct ScenarioResult {
  bool valid{};
  std::uint64_t trade_count{};
  std::uint64_t checksum{};
};

[[nodiscard]] ScenarioResult exercise_scenario(Scenario scenario, std::uint64_t samples);

struct DiagnosticResult {
  Histogram raw;
  Histogram corrected;
};

[[nodiscard]] DiagnosticResult synthetic_diagnostic(const std::vector<std::uint64_t>& values,
                                                    std::uint64_t expected_interval);

struct Config {
  Mode mode{Mode::open_loop};
  Scenario scenario{Scenario::crossing_limit};
  std::uint64_t samples{100'000U};
  std::uint64_t warmup{10'000U};
  std::uint64_t rate{100'000U};
  std::filesystem::path output_dir{"benchmark-results"};
  std::uint64_t diagnostic_interval_ns{10'000U};
  std::uint64_t diagnostic_stall_every{100U};
  std::uint64_t diagnostic_stall_ns{1'000'000U};
};

[[nodiscard]] std::optional<Config> parse_cli(const std::vector<std::string>& arguments,
                                              std::string* error = nullptr);

struct Summary {
  Mode mode{Mode::open_loop};
  Scenario scenario{Scenario::none};
  std::uint64_t count{};
  std::uint64_t minimum_ns{};
  std::uint64_t p50_ns{};
  std::uint64_t p90_ns{};
  std::uint64_t p99_ns{};
  std::uint64_t p99_9_ns{};
  std::uint64_t p99_99_ns{};
  std::uint64_t maximum_ns{};
  double mean_ns{};
  std::uint64_t requested_rate{};
  double achieved_rate{};
  std::uint64_t duration_ns{};
  std::uint64_t max_backlog{};
  std::uint64_t max_lateness_ns{};
  std::uint64_t backward_samples{};
  std::uint64_t migration_samples{};
  std::uint64_t invalid_samples{};
  std::uint64_t checksum{};
  ClaimScope claim_scope{ClaimScope::regression_only};
  std::string publication_reason{"operation_not_evaluated"};
  measurement::SelfCheckReport clock_report{};
};

[[nodiscard]] std::string summary_json(const Summary& summary);
[[nodiscard]] const char* mode_name(Mode mode) noexcept;
[[nodiscard]] const char* scenario_name(Scenario scenario) noexcept;
[[nodiscard]] const char* claim_scope_name(ClaimScope scope) noexcept;

struct RunResult {
  Summary summary;
  std::optional<Summary> corrected_summary;
  std::filesystem::path summary_path;
  std::filesystem::path raw_csv_path;
  std::filesystem::path percentile_path;
  std::filesystem::path corrected_summary_path;
  std::filesystem::path corrected_csv_path;
  std::filesystem::path corrected_percentile_path;
};

[[nodiscard]] std::optional<RunResult> run(const Config& config, std::string& error);

} // namespace matching_engine::benchmark
