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
  std::uint64_t numerator{};
  std::uint64_t denominator{};
  const auto first_gcd = std::gcd(index, rate);
  index /= first_gcd;
  rate /= first_gcd;
  const auto second_gcd = std::gcd(ratio.numerator, ratio.denominator);
  ratio.numerator /= second_gcd;
  ratio.denominator /= second_gcd;
  const auto third_gcd = std::gcd(kNanosecondsPerSecond, rate);
  const auto seconds = kNanosecondsPerSecond / third_gcd;
  rate /= third_gcd;
  const auto fourth_gcd = std::gcd(ratio.numerator, rate);
  ratio.numerator /= fourth_gcd;
  rate /= fourth_gcd;
  if (!checked_multiply(index, seconds, numerator) ||
      !checked_multiply(numerator, ratio.numerator, numerator) ||
      !checked_multiply(rate, ratio.denominator, denominator) || denominator == 0U) {
    return false;
  }
  offset = numerator / denominator;
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

  [[nodiscard]] bool execute(std::uint64_t event, std::uint64_t& checksum) noexcept {
    if (!valid_ || event >= events_) {
      return false;
    }
    const std::uint64_t maker_width = makers_per_event(scenario_);
    const OrderId taker_id{maker_count_ + event + 1U};
    const Quantity quantity{maker_width};
    const Price limit{scenario_ == Scenario::sweep_3_level
                          ? 103 + static_cast<std::int64_t>(event * maker_width)
                          : 101};
    const auto result =
        book_.submit_limit(taker_id, Side::buy, limit, quantity, TimeInForce::ioc, trades_);
    if (result.reject_reason != RejectReason::none || result.executed_quantity != quantity ||
        result.unfilled_quantity != Quantity{0U} || result.trade_count != maker_width) {
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
  bool valid_{};
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
  return output.good();
}

[[nodiscard]] bool write_percentiles(const std::filesystem::path& path,
                                     const Histogram& histogram) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "Percentile Value(ns)\n";
  for (const double percentile : {0.0, 50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
    output << std::fixed << std::setprecision(2) << percentile << ' '
           << histogram.percentile(percentile) << '\n';
  }
  return output.good();
}

[[nodiscard]] bool write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << text << '\n';
  return output.good();
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
  if (!summary.clock_report.clock_safe) {
    summary.clock_report.publication_reason =
        measurement::PublicationReason::clock_self_check_failed;
  } else if (!summary.clock_report.source_publishable) {
    summary.clock_report.publication_reason =
        measurement::PublicationReason::source_regression_only;
  } else {
    const auto granularity = summary.clock_report.effective_granularity_ticks;
    if (granularity > std::numeric_limits<std::uint64_t>::max() / 10U ||
        median_service_ticks < granularity * 10U) {
      summary.clock_report.publication_reason =
          measurement::PublicationReason::operation_below_resolution;
    } else {
      summary.clock_report.operation_percentiles_publishable = true;
      summary.clock_report.publication_reason = measurement::PublicationReason::qualified;
    }
  }
  summary.publication_reason =
      measurement::publication_reason_name(summary.clock_report.publication_reason);
  summary.claim_scope = summary.clock_report.operation_percentiles_publishable
                            ? ClaimScope::publishable_candidate
                            : ClaimScope::regression_only;
}

[[nodiscard]] std::string artifact_stem(const Config& config) {
  return std::string{mode_name(config.mode)} + "-" + scenario_name(config.scenario);
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
  while (current.ticks < intended.ticks &&
         (!capabilities.migration_detection || current.cpu_aux == intended.cpu_aux)) {
    current = read();
  }
  return current;
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
    result.valid = workload.execute(event, result.checksum);
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
  std::uint64_t total_events{};
  std::uint64_t maker_capacity{};
  if (!checked_add(config.samples, config.warmup, total_events) ||
      !checked_multiply(total_events, makers_per_event(config.scenario), maker_capacity) ||
      maker_capacity > std::numeric_limits<std::uint32_t>::max()) {
    return fail("requested run exceeds order-book capacity");
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
    config.scenario = Scenario::none;
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
  return scope == ClaimScope::publishable_candidate ? "publishable_candidate" : "regression_only";
}

std::string summary_json(const Summary& summary) {
  std::ostringstream output;
  output << std::setprecision(17) << '{' << "\"schema_version\":1,"
         << "\"mode\":\"" << mode_name(summary.mode) << "\","
         << "\"scenario\":\"" << scenario_name(summary.scenario) << "\","
         << "\"count\":" << summary.count << ',' << "\"min_ns\":" << summary.minimum_ns << ','
         << "\"p50_ns\":" << summary.p50_ns << ',' << "\"p90_ns\":" << summary.p90_ns << ','
         << "\"p99_ns\":" << summary.p99_ns << ',' << "\"p99_9_ns\":" << summary.p99_9_ns << ','
         << "\"p99_99_ns\":" << summary.p99_99_ns << ',' << "\"max_ns\":" << summary.maximum_ns
         << ',' << "\"mean_ns\":" << summary.mean_ns << ','
         << "\"requested_rate\":" << summary.requested_rate << ','
         << "\"achieved_rate\":" << summary.achieved_rate << ','
         << "\"duration_ns\":" << summary.duration_ns << ','
         << "\"max_backlog\":" << summary.max_backlog << ','
         << "\"max_lateness_ns\":" << summary.max_lateness_ns << ','
         << "\"backward_samples\":" << summary.backward_samples << ','
         << "\"migration_samples\":" << summary.migration_samples << ','
         << "\"invalid_samples\":" << summary.invalid_samples << ','
         << "\"checksum\":" << summary.checksum << ','
         << "\"operation_resolution_publication_reason\":\"" << summary.publication_reason << "\","
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
  const std::string stem = artifact_stem(config);

  if (config.mode == Mode::closed_loop_diagnostic) {
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
    result.summary.duration_ns = std::accumulate(values.begin(), values.end(), std::uint64_t{0U});
    result.summary.checksum = kChecksumBasis;
    for (const auto value : values) {
      checksum_value(result.summary.checksum, value);
    }
    evaluate_publication(result.summary, result.summary.p50_ns);
    result.summary.claim_scope = ClaimScope::regression_only;
    result.summary.publication_reason = "diagnostic_not_engine_claim";
    Summary corrected = result.summary;
    fill_histogram_summary(corrected, diagnostic.corrected);
    corrected.publication_reason = "diagnostic_corrected_not_engine_claim";
    result.corrected_summary = corrected;

    result.summary_path = config.output_dir / (stem + "-raw-summary.json");
    result.raw_csv_path = config.output_dir / (stem + "-raw-buckets.csv");
    result.percentile_path = config.output_dir / (stem + "-raw-percentiles.txt");
    result.corrected_summary_path = config.output_dir / (stem + "-corrected-summary.json");
    result.corrected_csv_path = config.output_dir / (stem + "-corrected-buckets.csv");
    result.corrected_percentile_path = config.output_dir / (stem + "-corrected-percentiles.txt");
    if (!write_text(result.summary_path, summary_json(result.summary)) ||
        !write_histogram_csv(result.raw_csv_path, diagnostic.raw) ||
        !write_percentiles(result.percentile_path, diagnostic.raw) ||
        !write_text(result.corrected_summary_path, summary_json(corrected)) ||
        !write_histogram_csv(result.corrected_csv_path, diagnostic.corrected) ||
        !write_percentiles(result.corrected_percentile_path, diagnostic.corrected)) {
      error = "failed to write diagnostic artifacts";
      return std::nullopt;
    }
    return result;
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
    if (!workload.execute(index, checksum)) {
      error = "warmup trade validation failed";
      return std::nullopt;
    }
  }

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
  const auto base = measurement::read_clock();
  const auto start_ns = measurement::read_steady_nanoseconds();
  for (std::uint64_t index = 0U; index < config.samples; ++index) {
    std::uint64_t intended_ticks{};
    if (!checked_add(base.ticks, (*schedule)[static_cast<std::size_t>(index)], intended_ticks)) {
      error = "intended arrival overflow";
      return std::nullopt;
    }
    const measurement::ClockSample intended{intended_ticks, base.cpu_aux};
    const auto actual_start = wait_until_intended({}, intended, capabilities);
    const bool valid_trade = workload.execute(config.warmup + index, checksum);
    const auto completion = measurement::read_clock();
    if (!valid_trade) {
      error = "measured trade validation failed";
      return std::nullopt;
    }
    const auto observation = observe_event(intended, actual_start, completion, capabilities);
    if (observation.status == measurement::ElapsedStatus::backward) {
      ++result.summary.backward_samples;
      ++result.summary.invalid_samples;
      continue;
    }
    if (observation.status == measurement::ElapsedStatus::migrated) {
      ++result.summary.migration_samples;
      ++result.summary.invalid_samples;
      continue;
    }
    std::uint64_t latency_ns{};
    std::uint64_t lateness_ns{};
    if (!measurement::ticks_to_nanoseconds(observation.latency_ticks, clock_report.ticks_per_ns,
                                           latency_ns) ||
        !measurement::ticks_to_nanoseconds(observation.lateness_ticks, clock_report.ticks_per_ns,
                                           lateness_ns) ||
        !latency.record(latency_ns) || !service_ticks.record(observation.service_ticks)) {
      error = "sample cannot be represented by histogram";
      return std::nullopt;
    }
    const auto elapsed_at_start = actual_start.ticks - base.ticks;
    const auto arrived = static_cast<std::uint64_t>(
        std::ranges::upper_bound(*schedule, elapsed_at_start) - schedule->begin());
    const auto backlog = arrived > index ? arrived - index : 0U;
    result.summary.max_backlog = std::max(result.summary.max_backlog, backlog);
    result.summary.max_lateness_ns = std::max(result.summary.max_lateness_ns, lateness_ns);
  }
  const auto end_ns = measurement::read_steady_nanoseconds();
  result.summary.duration_ns = end_ns >= start_ns ? end_ns - start_ns : 0U;
  result.summary.achieved_rate = result.summary.duration_ns == 0U
                                     ? 0.0
                                     : static_cast<double>(latency.count()) * 1'000'000'000.0 /
                                           static_cast<double>(result.summary.duration_ns);
  result.summary.checksum = checksum;
  fill_histogram_summary(result.summary, latency);
  evaluate_publication(result.summary, service_ticks.percentile(50.0));

  result.summary_path = config.output_dir / (stem + "-summary.json");
  result.raw_csv_path = config.output_dir / (stem + "-raw-buckets.csv");
  result.percentile_path = config.output_dir / (stem + "-raw-percentiles.txt");
  if (!write_text(result.summary_path, summary_json(result.summary)) ||
      !write_histogram_csv(result.raw_csv_path, latency) ||
      !write_percentiles(result.percentile_path, latency)) {
    error = "failed to write open-loop artifacts";
    return std::nullopt;
  }
  return result;
}

} // namespace matching_engine::benchmark
