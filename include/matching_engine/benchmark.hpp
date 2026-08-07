#pragma once

#include "matching_engine/measurement.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct hdr_histogram;

namespace matching_engine::benchmark {

inline constexpr std::uint64_t kMaximumSamples = 1'000'000U;
inline constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;
inline constexpr std::uint64_t kMaximumDiagnosticCorrectedCount = 10'000'000U;
inline constexpr std::uint64_t kBenchmarkMemoryBudgetBytes = 256U * 1024U * 1024U;

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
[[nodiscard]] std::uint64_t additional_backlog(std::span<const std::uint64_t> schedule,
                                               std::uint64_t current_index,
                                               std::uint64_t elapsed_ticks) noexcept;
[[nodiscard]] double achieved_completion_rate(std::uint64_t executed_operations,
                                              std::uint64_t first_completion_ticks,
                                              std::uint64_t last_completion_ticks,
                                              double ticks_per_ns) noexcept;

struct Bucket {
  std::uint64_t value{};
  std::uint64_t count{};
  constexpr bool operator==(const Bucket&) const noexcept = default;
};

struct PercentilePoint {
  double percentile{};
  std::uint64_t highest_equivalent_value{};
  std::uint64_t cumulative_count{};
  std::uint64_t total_count{};
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
  [[nodiscard]] std::vector<PercentilePoint> percentile_distribution() const;
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
  diagnostic_only,
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
[[nodiscard]] bool diagnostic_correction_count_upper_bound(std::uint64_t samples,
                                                           std::uint64_t expected_interval,
                                                           std::uint64_t stall_every,
                                                           std::uint64_t maximum_stall,
                                                           std::uint64_t& corrected_count) noexcept;

struct MemoryPlan {
  std::uint64_t planned_bytes{};
  std::uint64_t maker_count{};
  std::uint64_t price_level_count{};
};

[[nodiscard]] std::optional<MemoryPlan>
benchmark_memory_plan(Scenario scenario, std::uint64_t samples, std::uint64_t warmup) noexcept;

struct OperationCallbacks {
  void* context{};
  void (*submit)(void*, std::uint64_t) noexcept {};
  void (*capture)(void*, std::uint64_t) noexcept {};
  bool (*validate)(void*, std::uint64_t) noexcept {};
};

struct RawOpenLoopObservation {
  measurement::ClockSample intended{};
  measurement::ClockSample start{};
  measurement::ClockSample completion{};
};

enum class OpenLoopStatus : std::uint8_t {
  ok,
  invalid_configuration,
  cpu_migration,
  validation_failed,
  recording_failed,
  no_valid_samples,
};

struct OpenLoopStats {
  std::uint64_t executed_operations{};
  std::uint64_t valid_samples{};
  std::uint64_t backward_samples{};
  std::uint64_t migration_samples{};
  std::uint64_t invalid_samples{};
  std::uint64_t max_backlog{};
  std::uint64_t max_lateness_ns{};
  std::uint64_t first_completion_ticks{};
  std::uint64_t last_completion_ticks{};
};

[[nodiscard]] OpenLoopStatus
collect_open_loop(std::span<const std::uint64_t> schedule,
                  std::span<RawOpenLoopObservation> observations, measurement::ClockSample base,
                  measurement::ClockReader clock, measurement::ClockCapabilities capabilities,
                  double ticks_per_ns, OperationCallbacks operation, Histogram& latency,
                  Histogram& service_ticks, OpenLoopStats& stats) noexcept;
[[nodiscard]] const char* open_loop_status_message(OpenLoopStatus status) noexcept;

struct OperationResolution {
  std::uint64_t threshold_ticks{};
  bool resolved{};
};

[[nodiscard]] OperationResolution
evaluate_operation_resolution(std::uint64_t effective_granularity_ticks,
                              std::uint64_t median_service_ticks) noexcept;

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
  std::uint64_t valid_samples{};
  std::uint64_t executed_operations{};
  std::uint64_t minimum_ns{};
  std::uint64_t p50_ns{};
  std::uint64_t p90_ns{};
  std::uint64_t p99_ns{};
  std::uint64_t p99_9_ns{};
  std::uint64_t p99_99_ns{};
  std::uint64_t maximum_ns{};
  double mean_ns{};
  std::uint64_t requested_rate{};
  double achieved_completion_rate{};
  std::uint64_t duration_ns{};
  std::uint64_t completion_interval_ns{};
  std::uint64_t max_backlog{};
  std::uint64_t max_lateness_ns{};
  std::uint64_t backward_samples{};
  std::uint64_t migration_samples{};
  std::uint64_t invalid_samples{};
  std::uint64_t checksum{};
  std::uint64_t planned_memory_bytes{};
  std::uint64_t effective_granularity_ns{};
  std::uint64_t operation_median_ticks{};
  std::uint64_t operation_resolution_threshold_ticks{};
  ClaimScope claim_scope{ClaimScope::regression_only};
  std::string source_qualification_reason{"source_regression_only"};
  std::string operation_resolution_reason{"operation_not_evaluated"};
  measurement::SelfCheckReport clock_report{};
};

[[nodiscard]] bool populate_histogram_summary(Summary& summary,
                                              const Histogram& histogram) noexcept;
[[nodiscard]] std::optional<std::string> summary_json(const Summary& summary);
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
  std::filesystem::path final_directory;
};

[[nodiscard]] std::optional<RunResult> run(const Config& config, std::string& error);

} // namespace matching_engine::benchmark
