#include "matching_engine/benchmark.hpp"

#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <hdr/hdr_histogram.h>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace matching_engine::benchmark {
namespace {

constexpr std::uint64_t kHistogramMaximumNs = 3'600'000'000'000U;
constexpr int kHistogramSignificantFigures = 3;
constexpr Quantity kUnitQuantity{1U};
constexpr std::uint64_t kChecksumBasis = 14'695'981'039'346'656'037U;
constexpr std::uint64_t kChecksumPrime = 1'099'511'628'211U;

[[nodiscard]] bool checked_add(std::uint64_t left, std::uint64_t right,
                               std::uint64_t& result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(std::uint64_t left, std::uint64_t right,
                                    std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] bool exact_offset(std::uint64_t index, std::uint64_t rate, TickRatio ratio,
                                std::uint64_t& offset) noexcept {
  if (rate == 0U || ratio.numerator == 0U || ratio.denominator == 0U) {
    return false;
  }
  std::array<std::uint64_t, 3> numerators{index, kNanosecondsPerSecond, ratio.numerator};
  std::array<std::uint64_t, 2> denominators{rate, ratio.denominator};
  for (auto& numerator_factor : numerators) {
    for (auto& denominator_factor : denominators) {
      const auto divisor = std::gcd(numerator_factor, denominator_factor);
      numerator_factor /= divisor;
      denominator_factor /= divisor;
    }
  }
  std::uint64_t numerator{1U};
  std::uint64_t denominator{1U};
  for (const auto factor : numerators) {
    if (!checked_multiply(numerator, factor, numerator)) {
      return false;
    }
  }
  for (const auto factor : denominators) {
    if (!checked_multiply(denominator, factor, denominator) || denominator == 0U) {
      return false;
    }
  }
  offset = numerator / denominator;
  const auto remainder = numerator % denominator;
  if (remainder >= denominator - remainder && !checked_add(offset, 1U, offset)) {
    return false;
  }
  return true;
}

void checksum_value(std::uint64_t& checksum, std::uint64_t value) noexcept {
  checksum ^= value;
  checksum *= kChecksumPrime;
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool is_safe_output_path(const std::filesystem::path& path) {
  if (path.empty() || path == path.root_path()) {
    return false;
  }
  for (const auto& component : path) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint64_t makers_per_event(Scenario scenario) noexcept {
  return scenario == Scenario::sweep_3_level ? 3U : 1U;
}

class ScenarioWorkload {
public:
  ScenarioWorkload(Scenario scenario, std::uint64_t events)
      : scenario_(scenario), events_(events), maker_count_(events * makers_per_event(scenario)),
        book_(PriceDomain{Price{100}, static_cast<std::uint32_t>(scenario == Scenario::sweep_3_level
                                                                     ? maker_count_ + 1U
                                                                     : 2U)},
              static_cast<std::size_t>(maker_count_), Quantity{3U}),
        trades_(static_cast<std::size_t>(maker_count_)) {
    valid_ = preload();
  }

  [[nodiscard]] bool valid() const noexcept {
    return valid_;
  }

  void submit(std::uint64_t event) noexcept {
    submitted_event_ = event;
    if (!valid_ || event >= events_) {
      last_result_.reject_reason = RejectReason::invalid_handle;
      return;
    }
    const std::uint64_t maker_width = makers_per_event(scenario_);
    const OrderId taker_id{maker_count_ + event + 1U};
    const Quantity quantity{maker_width};
    const Price limit{scenario_ == Scenario::sweep_3_level
                          ? 103 + static_cast<std::int64_t>(event * maker_width)
                          : 101};
    last_result_ =
        book_.submit_limit(taker_id, Side::buy, limit, quantity, TimeInForce::ioc, trades_);
  }

  [[nodiscard]] bool validate(std::uint64_t event, std::uint64_t& checksum) noexcept {
    if (!valid_ || event >= events_) {
      return false;
    }
    const std::uint64_t maker_width = makers_per_event(scenario_);
    const OrderId taker_id{maker_count_ + event + 1U};
    const Quantity quantity{maker_width};
    if (submitted_event_ != event || last_result_.reject_reason != RejectReason::none ||
        last_result_.executed_quantity != quantity ||
        last_result_.unfilled_quantity != Quantity{0U} || last_result_.trade_count != maker_width) {
      return false;
    }
    for (std::uint64_t trade_index = 0U; trade_index < maker_width; ++trade_index) {
      const auto& trade = trades_[static_cast<std::size_t>(trade_index)];
      const Price expected_price{
          101 + static_cast<std::int64_t>(
                    scenario_ == Scenario::sweep_3_level ? event * maker_width + trade_index : 0U)};
      const OrderId expected_maker{scenario_ == Scenario::sweep_3_level
                                       ? (event * maker_width) + trade_index + 1U
                                       : event + 1U};
      if (trade.buy_id != taker_id || trade.sell_id != expected_maker ||
          trade.price != expected_price || trade.quantity != kUnitQuantity) {
        return false;
      }
      checksum_value(checksum, trade.buy_id.value());
      checksum_value(checksum, trade.sell_id.value());
      checksum_value(checksum, static_cast<std::uint64_t>(trade.price.ticks()));
      checksum_value(checksum, trade.quantity.value());
    }
    return true;
  }

private:
  [[nodiscard]] bool preload() noexcept {
    for (std::uint64_t event = 0U; event < events_; ++event) {
      for (std::uint64_t level = 0U; level < makers_per_event(scenario_); ++level) {
        const OrderId maker_id{scenario_ == Scenario::sweep_3_level
                                   ? (event * makers_per_event(scenario_)) + level + 1U
                                   : event + 1U};
        const Price price{
            101 + static_cast<std::int64_t>(scenario_ == Scenario::sweep_3_level
                                                ? event * makers_per_event(scenario_) + level
                                                : 0U)};
        const auto result = book_.submit_limit(maker_id, Side::sell, price, kUnitQuantity, trades_);
        if (result.reject_reason != RejectReason::none || result.trade_count != 0U) {
          return false;
        }
      }
    }
    return true;
  }

  Scenario scenario_;
  std::uint64_t events_;
  std::uint64_t maker_count_;
  OrderBook book_;
  std::vector<Trade> trades_;
  SubmitResult last_result_{RejectReason::invalid_handle, Quantity{0U}, Quantity{0U}, 0U, {}};
  std::uint64_t submitted_event_{std::numeric_limits<std::uint64_t>::max()};
  bool valid_{};
};

struct ScenarioOperation {
  ScenarioWorkload* workload{};
  std::uint64_t event_offset{};
  std::uint64_t* checksum{};

  static void submit(void* context, std::uint64_t event) noexcept {
    auto& operation = *static_cast<ScenarioOperation*>(context);
    operation.workload->submit(operation.event_offset + event);
  }

  static bool validate(void* context, std::uint64_t event) noexcept {
    auto& operation = *static_cast<ScenarioOperation*>(context);
    return operation.workload->validate(operation.event_offset + event, *operation.checksum);
  }
};

[[nodiscard]] bool write_histogram_csv(const std::filesystem::path& path,
                                       const Histogram& histogram) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "value,count\n";
  for (const auto bucket : histogram.recorded_buckets()) {
    output << bucket.value << ',' << bucket.count << '\n';
  }
  output.flush();
  output.close();
  return !output.fail();
}

[[nodiscard]] bool write_percentiles(const std::filesystem::path& path,
                                     const Histogram& histogram) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "percentile,highest_equivalent_value_ns,cumulative_count,total_count\n";
  for (const auto& point : histogram.percentile_distribution()) {
    output << std::setprecision(17) << point.percentile << ',' << point.highest_equivalent_value
           << ',' << point.cumulative_count << ',' << point.total_count << '\n';
  }
  output.flush();
  output.close();
  return !output.fail();
}

[[nodiscard]] bool write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << text << '\n';
  output.flush();
  output.close();
  return !output.fail();
}

void fill_histogram_summary(Summary& summary, const Histogram& histogram) {
  summary.count = histogram.count();
  summary.minimum_ns = histogram.minimum();
  summary.p50_ns = histogram.percentile(50.0);
  summary.p90_ns = histogram.percentile(90.0);
  summary.p99_ns = histogram.percentile(99.0);
  summary.p99_9_ns = histogram.percentile(99.9);
  summary.p99_99_ns = histogram.percentile(99.99);
  summary.maximum_ns = histogram.maximum();
  summary.mean_ns = histogram.mean();
}

void evaluate_publication(Summary& summary, std::uint64_t median_service_ticks) {
  summary.clock_report.operation_evaluated = true;
  summary.operation_median_ticks = median_service_ticks;
  const auto resolution = evaluate_operation_resolution(
      summary.clock_report.effective_granularity_ticks, median_service_ticks);
  summary.operation_resolution_threshold_ticks = resolution.threshold_ticks;
  summary.operation_resolution_reason =
      resolution.resolved ? "qualified" : "operation_below_resolution";
  summary.source_qualification_reason =
      summary.clock_report.source_publishable ? "qualified" : "source_regression_only";
  if (!summary.clock_report.clock_safe) {
    summary.clock_report.publication_reason =
        measurement::PublicationReason::clock_self_check_failed;
  } else if (!summary.clock_report.source_publishable) {
    summary.clock_report.publication_reason =
        measurement::PublicationReason::source_regression_only;
  } else if (!resolution.resolved) {
    summary.clock_report.publication_reason =
        measurement::PublicationReason::operation_below_resolution;
  } else {
    summary.clock_report.operation_percentiles_publishable = true;
    summary.clock_report.publication_reason = measurement::PublicationReason::qualified;
  }
  summary.claim_scope = summary.clock_report.operation_percentiles_publishable
                            ? ClaimScope::publishable_candidate
                            : ClaimScope::regression_only;
}

[[nodiscard]] std::string artifact_stem(const Config& config) {
  return std::string{mode_name(config.mode)} + "-" + scenario_name(config.scenario);
}

struct ArtifactTransaction {
  ArtifactTransaction(std::filesystem::path final_path) : final_directory(std::move(final_path)) {}
  ArtifactTransaction(ArtifactTransaction&& other) noexcept
      : final_directory(std::move(other.final_directory)),
        staging_directory(std::move(other.staging_directory)), committed(other.committed) {
    other.committed = true;
  }
  ArtifactTransaction& operator=(ArtifactTransaction&&) = delete;
  ArtifactTransaction(const ArtifactTransaction&) = delete;
  ArtifactTransaction& operator=(const ArtifactTransaction&) = delete;
  ~ArtifactTransaction() {
    if (!committed && !staging_directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(staging_directory, ignored);
    }
  }

  std::filesystem::path final_directory;
  std::filesystem::path staging_directory;
  bool committed{};
};

[[nodiscard]] std::optional<ArtifactTransaction>
begin_artifact_transaction(const std::filesystem::path& output_directory, std::string_view stem,
                           std::string& error) {
  ArtifactTransaction transaction{output_directory / (std::string{stem} + "-run")};
  std::error_code filesystem_error;
  if (std::filesystem::exists(transaction.final_directory, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "cannot inspect final artifact directory"
                             : "final artifact directory already exists";
    return std::nullopt;
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint32_t attempt = 0U; attempt < 100U; ++attempt) {
    transaction.staging_directory =
        output_directory / ("." + std::string{stem} + "-staging-" + std::to_string(nonce) + "-" +
                            std::to_string(attempt));
    if (std::filesystem::create_directory(transaction.staging_directory, filesystem_error)) {
      return transaction;
    }
    if (filesystem_error) {
      error = "cannot create unique artifact staging directory";
      return std::nullopt;
    }
  }
  error = "cannot create unique artifact staging directory";
  return std::nullopt;
}

void clean_staging(ArtifactTransaction& transaction) noexcept {
  std::error_code ignored;
  std::filesystem::remove_all(transaction.staging_directory, ignored);
  transaction.committed = true;
}

[[nodiscard]] bool commit_artifacts(ArtifactTransaction& transaction, std::string& error) {
  std::error_code filesystem_error;
  std::filesystem::rename(transaction.staging_directory, transaction.final_directory,
                          filesystem_error);
  if (filesystem_error) {
    clean_staging(transaction);
    error = "cannot atomically publish artifact directory";
    return false;
  }
  transaction.committed = true;
  return true;
}

} // namespace

std::optional<std::vector<std::uint64_t>> make_schedule(std::uint64_t samples, std::uint64_t rate,
                                                        TickRatio ticks_per_nanosecond) {
  if (samples == 0U || samples > kMaximumSamples || rate == 0U ||
      ticks_per_nanosecond.numerator == 0U || ticks_per_nanosecond.denominator == 0U) {
    return std::nullopt;
  }
  std::vector<std::uint64_t> offsets(static_cast<std::size_t>(samples));
  for (std::uint64_t index = 0U; index < samples; ++index) {
    if (!exact_offset(index, rate, ticks_per_nanosecond,
                      offsets[static_cast<std::size_t>(index)])) {
      return std::nullopt;
    }
  }
  return offsets;
}

EventObservation observe_event(measurement::ClockSample intended, measurement::ClockSample start,
                               measurement::ClockSample completion,
                               measurement::ClockCapabilities capabilities) noexcept {
  EventObservation result{};
  if (capabilities.migration_detection && start.cpu_aux != completion.cpu_aux) {
    result.status = measurement::ElapsedStatus::migrated;
    return result;
  }
  capabilities.migration_detection = false;
  std::uint64_t lateness{};
  std::uint64_t service{};
  std::uint64_t latency{};
  const auto lateness_status = measurement::elapsed_ticks(intended, start, capabilities, lateness);
  if (lateness_status != measurement::ElapsedStatus::valid) {
    result.status = lateness_status;
    return result;
  }
  const auto service_status = measurement::elapsed_ticks(start, completion, capabilities, service);
  if (service_status != measurement::ElapsedStatus::valid) {
    result.status = service_status;
    return result;
  }
  const auto latency_status =
      measurement::elapsed_ticks(intended, completion, capabilities, latency);
  if (latency_status != measurement::ElapsedStatus::valid) {
    result.status = latency_status;
    return result;
  }
  result.lateness_ticks = lateness;
  result.service_ticks = service;
  result.latency_ticks = latency;
  return result;
}

measurement::ClockSample wait_until_intended(measurement::ClockReader clock,
                                             measurement::ClockSample intended,
                                             measurement::ClockCapabilities capabilities) noexcept {
  const auto read = [&clock] {
    return clock.read == nullptr ? measurement::read_clock() : clock.read(clock.context);
  };
  auto current = read();
  static_cast<void>(capabilities);
  while (current.ticks < intended.ticks) {
    current = read();
  }
  return current;
}

std::uint64_t additional_backlog(std::span<const std::uint64_t> schedule,
                                 std::uint64_t current_index,
                                 std::uint64_t elapsed_ticks) noexcept {
  if (current_index >= schedule.size()) {
    return 0U;
  }
  const auto next_index = static_cast<std::size_t>(current_index) + 1U;
  const auto first_pending = schedule.begin() + static_cast<std::ptrdiff_t>(next_index);
  const auto arrived_end = std::ranges::upper_bound(first_pending, schedule.end(), elapsed_ticks);
  return static_cast<std::uint64_t>(arrived_end - first_pending);
}

double achieved_completion_rate(std::uint64_t executed_operations,
                                std::uint64_t first_completion_ticks,
                                std::uint64_t last_completion_ticks, double ticks_per_ns) noexcept {
  if (executed_operations < 2U || last_completion_ticks <= first_completion_ticks ||
      !std::isfinite(ticks_per_ns) || ticks_per_ns <= 0.0) {
    return 0.0;
  }
  const auto intervals = static_cast<double>(executed_operations - 1U);
  const auto elapsed_ticks = static_cast<double>(last_completion_ticks - first_completion_ticks);
  return intervals * kNanosecondsPerSecond * ticks_per_ns / elapsed_ticks;
}

OperationResolution evaluate_operation_resolution(std::uint64_t effective_granularity_ticks,
                                                  std::uint64_t median_service_ticks) noexcept {
  if (effective_granularity_ticks > std::numeric_limits<std::uint64_t>::max() / 10U) {
    return {.threshold_ticks = std::numeric_limits<std::uint64_t>::max(), .resolved = false};
  }
  const auto threshold = effective_granularity_ticks * 10U;
  return {.threshold_ticks = threshold, .resolved = median_service_ticks >= threshold};
}

bool collect_open_loop(std::span<const std::uint64_t> schedule, measurement::ClockSample base,
                       measurement::ClockReader clock, measurement::ClockCapabilities capabilities,
                       double ticks_per_ns, OperationCallbacks operation, Histogram& latency,
                       Histogram& service_ticks, OpenLoopStats& stats) noexcept {
  if (schedule.empty() || operation.submit == nullptr || operation.validate == nullptr ||
      !latency.valid() || !service_ticks.valid()) {
    return false;
  }
  const auto read = [&clock] {
    return clock.read == nullptr ? measurement::read_clock() : clock.read(clock.context);
  };
  for (std::uint64_t index = 0U; index < schedule.size(); ++index) {
    std::uint64_t intended_ticks{};
    if (!checked_add(base.ticks, schedule[static_cast<std::size_t>(index)], intended_ticks)) {
      return false;
    }
    const measurement::ClockSample intended{intended_ticks, 0U};
    const auto actual_start = wait_until_intended(clock, intended, capabilities);
    operation.submit(operation.context, index);
    const auto completion = read();
    ++stats.executed_operations;
    if (stats.executed_operations == 1U) {
      stats.first_completion_ticks = completion.ticks;
    }
    stats.last_completion_ticks = completion.ticks;
    if (!operation.validate(operation.context, index)) {
      return false;
    }

    if (actual_start.ticks >= base.ticks) {
      const auto elapsed_at_start = actual_start.ticks - base.ticks;
      stats.max_backlog =
          std::max(stats.max_backlog, additional_backlog(schedule, index, elapsed_at_start));
    }
    std::uint64_t lateness_ns{};
    if (actual_start.ticks >= intended.ticks) {
      const auto lateness_ticks = actual_start.ticks - intended.ticks;
      if (!measurement::ticks_to_nanoseconds(lateness_ticks, ticks_per_ns, lateness_ns)) {
        return false;
      }
      stats.max_lateness_ns = std::max(stats.max_lateness_ns, lateness_ns);
    }
    const auto observation = observe_event(intended, actual_start, completion, capabilities);
    if (observation.status == measurement::ElapsedStatus::backward) {
      ++stats.backward_samples;
      ++stats.invalid_samples;
      continue;
    }
    if (observation.status == measurement::ElapsedStatus::migrated) {
      ++stats.migration_samples;
      ++stats.invalid_samples;
      continue;
    }
    std::uint64_t latency_ns{};
    if (!measurement::ticks_to_nanoseconds(observation.latency_ticks, ticks_per_ns, latency_ns) ||
        !latency.record(latency_ns) || !service_ticks.record(observation.service_ticks)) {
      return false;
    }
    ++stats.valid_samples;
  }
  return true;
}

void Histogram::Deleter::operator()(hdr_histogram* histogram) const noexcept {
  if (histogram != nullptr) {
    hdr_close(histogram);
  }
}

Histogram::Histogram(std::uint64_t lowest, std::uint64_t highest, int significant_figures) {
  if (lowest > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      highest > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return;
  }
  hdr_histogram* histogram{};
  if (hdr_init(static_cast<std::int64_t>(lowest), static_cast<std::int64_t>(highest),
               significant_figures, &histogram) == 0) {
    histogram_.reset(histogram);
  }
}

Histogram::Histogram(Histogram&&) noexcept = default;
Histogram& Histogram::operator=(Histogram&&) noexcept = default;

bool Histogram::valid() const noexcept {
  return histogram_ != nullptr;
}

bool Histogram::record(std::uint64_t value) noexcept {
  return histogram_ != nullptr &&
         value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
         hdr_record_value(histogram_.get(), static_cast<std::int64_t>(value));
}

bool Histogram::record_corrected(std::uint64_t value, std::uint64_t expected_interval) noexcept {
  return histogram_ != nullptr && expected_interval != 0U &&
         value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
         expected_interval <=
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
         hdr_record_corrected_value(histogram_.get(), static_cast<std::int64_t>(value),
                                    static_cast<std::int64_t>(expected_interval));
}

std::uint64_t Histogram::count() const noexcept {
  return histogram_ == nullptr || histogram_->total_count < 0
             ? 0U
             : static_cast<std::uint64_t>(histogram_->total_count);
}

std::uint64_t Histogram::minimum() const noexcept {
  const auto value = histogram_ == nullptr ? 0 : hdr_min(histogram_.get());
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

std::uint64_t Histogram::maximum() const noexcept {
  const auto value = histogram_ == nullptr ? 0 : hdr_max(histogram_.get());
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

std::uint64_t Histogram::percentile(double percentile_value) const noexcept {
  const auto value =
      histogram_ == nullptr ? 0 : hdr_value_at_percentile(histogram_.get(), percentile_value);
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

double Histogram::mean() const noexcept {
  return histogram_ == nullptr ? 0.0 : hdr_mean(histogram_.get());
}

std::vector<Bucket> Histogram::recorded_buckets() const {
  std::vector<Bucket> buckets;
  if (histogram_ == nullptr) {
    return buckets;
  }
  hdr_iter iterator{};
  hdr_iter_recorded_init(&iterator, histogram_.get());
  while (hdr_iter_next(&iterator)) {
    if (iterator.count > 0) {
      buckets.push_back({.value = static_cast<std::uint64_t>(iterator.value),
                         .count = static_cast<std::uint64_t>(
                             iterator.specifics.recorded.count_added_in_this_iteration_step)});
    }
  }
  return buckets;
}

std::vector<PercentilePoint> Histogram::percentile_distribution() const {
  std::vector<PercentilePoint> distribution;
  if (histogram_ == nullptr) {
    return distribution;
  }
  hdr_iter iterator{};
  hdr_iter_percentile_init(&iterator, histogram_.get(), 5);
  while (hdr_iter_next(&iterator)) {
    distribution.push_back(
        {.percentile = iterator.specifics.percentiles.percentile,
         .highest_equivalent_value = static_cast<std::uint64_t>(iterator.highest_equivalent_value),
         .cumulative_count = static_cast<std::uint64_t>(iterator.cumulative_count),
         .total_count = count()});
  }
  return distribution;
}

hdr_histogram* Histogram::native() noexcept {
  return histogram_.get();
}

ScenarioResult exercise_scenario(Scenario scenario, std::uint64_t samples) {
  if (scenario == Scenario::none || samples == 0U || samples > kMaximumSamples ||
      samples > std::numeric_limits<std::uint32_t>::max() / makers_per_event(scenario)) {
    return {};
  }
  ScenarioWorkload workload{scenario, samples};
  ScenarioResult result{.valid = workload.valid(), .checksum = kChecksumBasis};
  for (std::uint64_t event = 0U; result.valid && event < samples; ++event) {
    workload.submit(event);
    result.valid = workload.validate(event, result.checksum);
    result.trade_count += makers_per_event(scenario);
  }
  return result;
}

DiagnosticResult synthetic_diagnostic(const std::vector<std::uint64_t>& values,
                                      std::uint64_t expected_interval) {
  DiagnosticResult result{Histogram{1U, kHistogramMaximumNs, kHistogramSignificantFigures},
                          Histogram{1U, kHistogramMaximumNs, kHistogramSignificantFigures}};
  for (const auto value : values) {
    if (!result.raw.record(value) || !result.corrected.record_corrected(value, expected_interval)) {
      return {Histogram{0U, 0U, 0}, Histogram{0U, 0U, 0}};
    }
  }
  return result;
}

bool diagnostic_correction_count_upper_bound(std::uint64_t samples, std::uint64_t expected_interval,
                                             std::uint64_t stall_every, std::uint64_t maximum_stall,
                                             std::uint64_t& corrected_count) noexcept {
  if (samples == 0U || expected_interval == 0U || stall_every == 0U ||
      maximum_stall < expected_interval) {
    return false;
  }
  const auto stall_count = samples / stall_every;
  const auto corrected_per_stall = maximum_stall / expected_interval;
  const auto additional_per_stall = corrected_per_stall > 0U ? corrected_per_stall - 1U : 0U;
  std::uint64_t additional{};
  if (!checked_multiply(stall_count, additional_per_stall, additional) ||
      !checked_add(samples, additional, corrected_count)) {
    return false;
  }
  return corrected_count <= kMaximumDiagnosticCorrectedCount;
}

std::optional<MemoryPlan> benchmark_memory_plan(Scenario scenario, std::uint64_t samples,
                                                std::uint64_t warmup) noexcept {
  if (scenario == Scenario::none || samples == 0U || warmup == 0U) {
    return std::nullopt;
  }
  std::uint64_t total_events{};
  std::uint64_t maker_count{};
  if (!checked_add(samples, warmup, total_events) ||
      !checked_multiply(total_events, makers_per_event(scenario), maker_count)) {
    return std::nullopt;
  }
  const auto price_level_count = scenario == Scenario::sweep_3_level ? maker_count + 1U : 2U;
  // GLOBAL SYSTEM INSTRUCTIONS BY DIV: This conservative estimate intentionally rounds the
  // private arena slot up to 64 bytes. Expose an exact core estimator if the slot ever exceeds it.
  // ponytail: The checked 256 MiB ceiling avoids a benchmark-only core bypass or VM trick.
  constexpr std::uint64_t kConservativeArenaSlotBytes = 64U;
  constexpr std::uint64_t kHistogramStorageAllowance = 16ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t kPerMakerBytes =
      sizeof(Trade) + kConservativeArenaSlotBytes + sizeof(std::uint32_t);
  constexpr std::uint64_t kPerPriceLevelBytes =
      (2U * sizeof(PriceLevel)) + sizeof(std::uint32_t) + 1U;
  std::uint64_t maker_bytes{};
  std::uint64_t price_bytes{};
  std::uint64_t schedule_bytes{};
  std::uint64_t planned_bytes{kHistogramStorageAllowance};
  if (!checked_multiply(maker_count, kPerMakerBytes, maker_bytes) ||
      !checked_multiply(price_level_count, kPerPriceLevelBytes, price_bytes) ||
      !checked_multiply(samples, sizeof(std::uint64_t), schedule_bytes) ||
      !checked_add(planned_bytes, maker_bytes, planned_bytes) ||
      !checked_add(planned_bytes, price_bytes, planned_bytes) ||
      !checked_add(planned_bytes, schedule_bytes, planned_bytes) ||
      planned_bytes > kBenchmarkMemoryBudgetBytes) {
    return std::nullopt;
  }
  return MemoryPlan{
      .planned_bytes = planned_bytes,
      .maker_count = maker_count,
      .price_level_count = price_level_count,
  };
}

std::optional<Config> parse_cli(const std::vector<std::string>& arguments, std::string* error) {
  Config config{};
  bool scenario_set = false;
  bool rate_set = false;
  bool warmup_set = false;
  std::vector<std::string_view> seen_options;
  const auto fail = [&](std::string message) -> std::optional<Config> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  for (std::size_t index = 0U; index < arguments.size(); index += 2U) {
    if (index + 1U >= arguments.size()) {
      return fail("every option requires one value");
    }
    const std::string_view option = arguments[index];
    const std::string_view value = arguments[index + 1U];
    if (std::ranges::find(seen_options, option) != seen_options.end()) {
      return fail("duplicate option");
    }
    seen_options.push_back(option);
    std::uint64_t number{};
    if (option == "--mode") {
      if (value == "open-loop") {
        config.mode = Mode::open_loop;
      } else if (value == "closed-loop-diagnostic") {
        config.mode = Mode::closed_loop_diagnostic;
      } else {
        return fail("invalid --mode");
      }
    } else if (option == "--scenario") {
      scenario_set = true;
      if (value == "crossing-limit") {
        config.scenario = Scenario::crossing_limit;
      } else if (value == "sweep-3-level") {
        config.scenario = Scenario::sweep_3_level;
      } else {
        return fail("invalid --scenario");
      }
    } else if (option == "--output-dir") {
      config.output_dir = std::filesystem::path{value};
    } else if (option == "--samples" || option == "--warmup" || option == "--rate" ||
               option == "--diagnostic-interval-ns" || option == "--diagnostic-stall-every" ||
               option == "--diagnostic-stall-ns") {
      if (!parse_u64(value, number) || number == 0U) {
        return fail("numeric options require a positive base-10 integer");
      }
      if (option == "--samples") {
        config.samples = number;
      } else if (option == "--warmup") {
        warmup_set = true;
        config.warmup = number;
      } else if (option == "--rate") {
        rate_set = true;
        config.rate = number;
      } else if (option == "--diagnostic-interval-ns") {
        config.diagnostic_interval_ns = number;
      } else if (option == "--diagnostic-stall-every") {
        config.diagnostic_stall_every = number;
      } else {
        config.diagnostic_stall_ns = number;
      }
    } else {
      return fail("unknown option");
    }
  }
  if (config.samples > kMaximumSamples || config.warmup > kMaximumSamples) {
    return fail("samples and warmup are capped at 1000000");
  }
  if (config.rate > kNanosecondsPerSecond) {
    return fail("rate cannot exceed one event per nanosecond");
  }
  if (config.diagnostic_interval_ns > kHistogramMaximumNs ||
      config.diagnostic_stall_ns > kHistogramMaximumNs) {
    return fail("diagnostic values exceed the histogram range");
  }
  if (!is_safe_output_path(config.output_dir)) {
    return fail("output directory must be non-empty, non-root, and contain no parent traversal");
  }
  const bool diagnostic_option = std::ranges::any_of(
      arguments, [](const std::string& argument) { return argument.starts_with("--diagnostic-"); });
  if (config.mode == Mode::open_loop && diagnostic_option) {
    return fail("diagnostic options require closed-loop-diagnostic mode");
  }
  if (config.mode == Mode::closed_loop_diagnostic && scenario_set) {
    return fail("closed-loop-diagnostic does not accept an engine scenario");
  }
  if (config.mode == Mode::closed_loop_diagnostic) {
    if (rate_set || warmup_set) {
      return fail("closed-loop-diagnostic does not accept rate or warmup");
    }
    if (config.diagnostic_stall_every > config.samples ||
        config.diagnostic_stall_ns <= config.diagnostic_interval_ns) {
      return fail("diagnostic stall must occur and exceed the expected interval");
    }
    std::uint64_t corrected_count{};
    if (!diagnostic_correction_count_upper_bound(config.samples, config.diagnostic_interval_ns,
                                                 config.diagnostic_stall_every,
                                                 config.diagnostic_stall_ns, corrected_count)) {
      return fail("diagnostic correction exceeds the 10000000-record work bound");
    }
    config.scenario = Scenario::none;
  } else if (!benchmark_memory_plan(config.scenario, config.samples, config.warmup).has_value()) {
    return fail("requested run exceeds the 256 MiB benchmark memory budget");
  }
  return config;
}

const char* mode_name(Mode mode) noexcept {
  return mode == Mode::open_loop ? "open-loop" : "closed-loop-diagnostic";
}

const char* scenario_name(Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::crossing_limit:
    return "crossing-limit";
  case Scenario::sweep_3_level:
    return "sweep-3-level";
  case Scenario::none:
    return "none";
  }
  return "none";
}

const char* claim_scope_name(ClaimScope scope) noexcept {
  switch (scope) {
  case ClaimScope::regression_only:
    return "regression_only";
  case ClaimScope::publishable_candidate:
    return "publishable_candidate";
  case ClaimScope::diagnostic_only:
    return "diagnostic_only";
  }
  return "regression_only";
}

std::string summary_json(const Summary& summary) {
  std::ostringstream output;
  output << std::setprecision(17) << '{' << "\"schema_version\":1,"
         << "\"mode\":\"" << mode_name(summary.mode) << "\","
         << "\"scenario\":\"" << scenario_name(summary.scenario) << "\","
         << "\"count\":" << summary.count << ','
         << "\"executed_operations\":" << summary.executed_operations << ','
         << "\"min_ns\":" << summary.minimum_ns << ',' << "\"p50_ns\":" << summary.p50_ns << ','
         << "\"p90_ns\":" << summary.p90_ns << ',' << "\"p99_ns\":" << summary.p99_ns << ','
         << "\"p99_9_ns\":" << summary.p99_9_ns << ',' << "\"p99_99_ns\":" << summary.p99_99_ns
         << ',' << "\"max_ns\":" << summary.maximum_ns << ',' << "\"mean_ns\":" << summary.mean_ns
         << ',' << "\"requested_rate\":" << summary.requested_rate << ','
         << "\"achieved_completion_rate\":" << summary.achieved_completion_rate << ','
         << "\"duration_ns\":" << summary.duration_ns << ','
         << "\"completion_interval_ns\":" << summary.completion_interval_ns << ','
         << "\"max_backlog\":" << summary.max_backlog << ','
         << "\"max_lateness_ns\":" << summary.max_lateness_ns << ','
         << "\"backward_samples\":" << summary.backward_samples << ','
         << "\"migration_samples\":" << summary.migration_samples << ','
         << "\"invalid_samples\":" << summary.invalid_samples << ','
         << "\"checksum\":" << summary.checksum << ','
         << "\"planned_memory_bytes\":" << summary.planned_memory_bytes << ','
         << "\"effective_granularity_ns\":" << summary.effective_granularity_ns << ','
         << "\"operation_median_ticks\":" << summary.operation_median_ticks << ','
         << "\"operation_resolution_threshold_ticks\":"
         << summary.operation_resolution_threshold_ticks << ','
         << "\"source_qualification_reason\":\"" << summary.source_qualification_reason << "\","
         << "\"operation_resolution_reason\":\"" << summary.operation_resolution_reason << "\","
         << "\"claim_scope\":\"" << claim_scope_name(summary.claim_scope) << "\","
         << "\"clock_report\":{"
         << "\"source\":\"" << measurement::source_name(summary.clock_report.source) << "\","
         << "\"clock_safe\":" << (summary.clock_report.clock_safe ? "true" : "false") << ','
         << "\"source_publishable\":"
         << (summary.clock_report.source_publishable ? "true" : "false") << ','
         << "\"operation_evaluated\":"
         << (summary.clock_report.operation_evaluated ? "true" : "false") << ','
         << "\"operation_percentiles_publishable\":"
         << (summary.clock_report.operation_percentiles_publishable ? "true" : "false") << ','
         << "\"effective_granularity_ticks\":" << summary.clock_report.effective_granularity_ticks
         << ',' << "\"median_overhead_ticks\":" << summary.clock_report.median_overhead_ticks << ','
         << "\"p99_overhead_ticks\":" << summary.clock_report.p99_overhead_ticks << ','
         << "\"ticks_per_ns\":" << summary.clock_report.ticks_per_ns << ','
         << "\"calibration_uncertainty\":" << summary.clock_report.calibration_uncertainty << ','
         << "\"self_check_reason\":\""
         << measurement::self_check_reason_name(summary.clock_report.self_check_reason) << "\","
         << "\"publication_reason\":\""
         << measurement::publication_reason_name(summary.clock_report.publication_reason) << "\"}}";
  return output.str();
}

std::optional<RunResult> run(const Config& config, std::string& error) {
  const auto capabilities = measurement::clock_capabilities();
  auto clock_report =
      measurement::run_self_check({}, {}, capabilities, measurement::SelfCheckConfig{});
  if (!clock_report.clock_safe) {
    error = std::string{"unsafe clock: "} +
            measurement::self_check_reason_name(clock_report.self_check_reason);
    return std::nullopt;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(config.output_dir, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(config.output_dir, filesystem_error)) {
    error = "cannot create output directory";
    return std::nullopt;
  }

  RunResult result{};
  result.summary.mode = config.mode;
  result.summary.scenario = config.scenario;
  result.summary.requested_rate = config.mode == Mode::open_loop ? config.rate : 0U;
  result.summary.clock_report = clock_report;
  if (!measurement::ticks_to_nanoseconds(clock_report.effective_granularity_ticks,
                                         clock_report.ticks_per_ns,
                                         result.summary.effective_granularity_ns)) {
    error = "clock granularity cannot be represented in nanoseconds";
    return std::nullopt;
  }
  const std::string stem = artifact_stem(config);
  const auto final_directory = config.output_dir / (stem + "-run");
  if (std::filesystem::exists(final_directory, filesystem_error) || filesystem_error) {
    error = filesystem_error ? "cannot inspect final artifact directory"
                             : "final artifact directory already exists";
    return std::nullopt;
  }

  if (config.mode == Mode::closed_loop_diagnostic) {
    std::uint64_t corrected_count{};
    if (!diagnostic_correction_count_upper_bound(config.samples, config.diagnostic_interval_ns,
                                                 config.diagnostic_stall_every,
                                                 config.diagnostic_stall_ns, corrected_count)) {
      error = "diagnostic correction exceeds the 10000000-record work bound";
      return std::nullopt;
    }
    std::vector<std::uint64_t> values(static_cast<std::size_t>(config.samples),
                                      config.diagnostic_interval_ns);
    for (std::uint64_t index = config.diagnostic_stall_every - 1U; index < config.samples;) {
      values[static_cast<std::size_t>(index)] = config.diagnostic_stall_ns;
      if (index > std::numeric_limits<std::uint64_t>::max() - config.diagnostic_stall_every) {
        break;
      }
      index += config.diagnostic_stall_every;
    }
    auto diagnostic = synthetic_diagnostic(values, config.diagnostic_interval_ns);
    if (!diagnostic.raw.valid() || !diagnostic.corrected.valid()) {
      error = "diagnostic histogram recording failed";
      return std::nullopt;
    }
    fill_histogram_summary(result.summary, diagnostic.raw);
    result.summary.executed_operations = config.samples;
    result.summary.duration_ns = std::accumulate(values.begin(), values.end(), std::uint64_t{0U});
    result.summary.checksum = kChecksumBasis;
    for (const auto value : values) {
      checksum_value(result.summary.checksum, value);
    }
    result.summary.claim_scope = ClaimScope::diagnostic_only;
    result.summary.source_qualification_reason = "diagnostic_only";
    result.summary.operation_resolution_reason = "operation_not_evaluated";
    result.summary.clock_report.operation_evaluated = false;
    result.summary.clock_report.operation_percentiles_publishable = false;
    result.summary.clock_report.publication_reason =
        measurement::PublicationReason::operation_not_evaluated;
    Summary corrected = result.summary;
    fill_histogram_summary(corrected, diagnostic.corrected);
    result.corrected_summary = corrected;

    auto transaction = begin_artifact_transaction(config.output_dir, stem, error);
    if (!transaction.has_value()) {
      return std::nullopt;
    }
    const auto raw_summary_name = stem + "-raw-summary.json";
    const auto raw_csv_name = stem + "-raw-buckets.csv";
    const auto raw_percentile_name = stem + "-raw-percentiles.txt";
    const auto corrected_summary_name = stem + "-corrected-summary.json";
    const auto corrected_csv_name = stem + "-corrected-buckets.csv";
    const auto corrected_percentile_name = stem + "-corrected-percentiles.txt";
    if (!write_text(transaction->staging_directory / raw_summary_name,
                    summary_json(result.summary)) ||
        !write_histogram_csv(transaction->staging_directory / raw_csv_name, diagnostic.raw) ||
        !write_percentiles(transaction->staging_directory / raw_percentile_name, diagnostic.raw) ||
        !write_text(transaction->staging_directory / corrected_summary_name,
                    summary_json(corrected)) ||
        !write_histogram_csv(transaction->staging_directory / corrected_csv_name,
                             diagnostic.corrected) ||
        !write_percentiles(transaction->staging_directory / corrected_percentile_name,
                           diagnostic.corrected)) {
      clean_staging(*transaction);
      error = "failed to write diagnostic artifacts";
      return std::nullopt;
    }
    if (!commit_artifacts(*transaction, error)) {
      return std::nullopt;
    }
    result.final_directory = transaction->final_directory;
    result.summary_path = result.final_directory / raw_summary_name;
    result.raw_csv_path = result.final_directory / raw_csv_name;
    result.percentile_path = result.final_directory / raw_percentile_name;
    result.corrected_summary_path = result.final_directory / corrected_summary_name;
    result.corrected_csv_path = result.final_directory / corrected_csv_name;
    result.corrected_percentile_path = result.final_directory / corrected_percentile_name;
    return result;
  }

  const auto memory_plan = benchmark_memory_plan(config.scenario, config.samples, config.warmup);
  if (!memory_plan.has_value()) {
    error = "requested run exceeds the 256 MiB benchmark memory budget";
    return std::nullopt;
  }
  result.summary.planned_memory_bytes = memory_plan->planned_bytes;
  const long double scaled_ratio =
      static_cast<long double>(clock_report.ticks_per_ns) * 1'000'000'000.0L;
  if (!std::isfinite(scaled_ratio) || scaled_ratio < 1.0L ||
      scaled_ratio > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    error = "calibrated tick ratio is not representable";
    return std::nullopt;
  }
  const TickRatio ratio{static_cast<std::uint64_t>(std::llround(scaled_ratio)), 1'000'000'000U};
  const auto schedule = make_schedule(config.samples, config.rate, ratio);
  if (!schedule.has_value()) {
    error = "arrival schedule overflow";
    return std::nullopt;
  }

  Histogram latency{1U, kHistogramMaximumNs, kHistogramSignificantFigures};
  Histogram service_ticks{1U, std::numeric_limits<std::int64_t>::max(),
                          kHistogramSignificantFigures};
  if (!latency.valid() || !service_ticks.valid()) {
    error = "histogram initialization failed";
    return std::nullopt;
  }
  std::uint64_t total_events{};
  if (!checked_add(config.warmup, config.samples, total_events)) {
    error = "event count overflow";
    return std::nullopt;
  }
  ScenarioWorkload workload{config.scenario, total_events};
  if (!workload.valid()) {
    error = "maker preload failed";
    return std::nullopt;
  }
  std::uint64_t checksum = kChecksumBasis;
  for (std::uint64_t index = 0U; index < config.warmup; ++index) {
    workload.submit(index);
    if (!workload.validate(index, checksum)) {
      error = "warmup trade validation failed";
      return std::nullopt;
    }
  }

  const auto base = measurement::read_clock();
  const auto start_ns = measurement::read_steady_nanoseconds();
  ScenarioOperation operation{&workload, config.warmup, &checksum};
  OpenLoopStats stats{};
  if (!collect_open_loop(*schedule, base, {}, capabilities, clock_report.ticks_per_ns,
                         {&operation, &ScenarioOperation::submit, &ScenarioOperation::validate},
                         latency, service_ticks, stats)) {
    error = "measured operation, validation, or sample recording failed";
    return std::nullopt;
  }
  const auto end_ns = measurement::read_steady_nanoseconds();
  result.summary.duration_ns = end_ns >= start_ns ? end_ns - start_ns : 0U;
  result.summary.achieved_completion_rate =
      achieved_completion_rate(stats.executed_operations, stats.first_completion_ticks,
                               stats.last_completion_ticks, clock_report.ticks_per_ns);
  if (stats.executed_operations >= 2U &&
      stats.last_completion_ticks > stats.first_completion_ticks &&
      !measurement::ticks_to_nanoseconds(stats.last_completion_ticks - stats.first_completion_ticks,
                                         clock_report.ticks_per_ns,
                                         result.summary.completion_interval_ns)) {
    error = "completion interval cannot be represented";
    return std::nullopt;
  }
  result.summary.max_backlog = stats.max_backlog;
  result.summary.executed_operations = stats.executed_operations;
  result.summary.max_lateness_ns = stats.max_lateness_ns;
  result.summary.backward_samples = stats.backward_samples;
  result.summary.migration_samples = stats.migration_samples;
  result.summary.invalid_samples = stats.invalid_samples;
  result.summary.checksum = checksum;
  fill_histogram_summary(result.summary, latency);
  evaluate_publication(result.summary, service_ticks.percentile(50.0));

  auto transaction = begin_artifact_transaction(config.output_dir, stem, error);
  if (!transaction.has_value()) {
    return std::nullopt;
  }
  const auto summary_name = stem + "-summary.json";
  const auto csv_name = stem + "-raw-buckets.csv";
  const auto percentile_name = stem + "-raw-percentiles.txt";
  if (!write_text(transaction->staging_directory / summary_name, summary_json(result.summary)) ||
      !write_histogram_csv(transaction->staging_directory / csv_name, latency) ||
      !write_percentiles(transaction->staging_directory / percentile_name, latency)) {
    clean_staging(*transaction);
    error = "failed to write open-loop artifacts";
    return std::nullopt;
  }
  if (!commit_artifacts(*transaction, error)) {
    return std::nullopt;
  }
  result.final_directory = transaction->final_directory;
  result.summary_path = result.final_directory / summary_name;
  result.raw_csv_path = result.final_directory / csv_name;
  result.percentile_path = result.final_directory / percentile_name;
  return result;
}

} // namespace matching_engine::benchmark
