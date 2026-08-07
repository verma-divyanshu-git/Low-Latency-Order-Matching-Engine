#include "matching_engine/throughput_gate.hpp"

#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace matching_engine::throughput_gate {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;
constexpr std::uint64_t kChecksumBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kChecksumPrime = 1'099'511'628'211ULL;

struct CapturedEvent {
  SubmitResult result{RejectReason::invalid_handle, Quantity{0U}, Quantity{0U}, 0U, {}};
  Trade trade{};
};

void checksum_value(std::uint64_t& checksum, std::uint64_t value) noexcept {
  checksum ^= value;
  checksum *= kChecksumPrime;
}

class CrossingBatch {
public:
  explicit CrossingBatch(std::uint64_t samples)
      : samples_(samples),
        book_(PriceDomain{Price{100}, 2U}, static_cast<std::size_t>(samples), Quantity{1U}),
        trade_buffer_(static_cast<std::size_t>(samples)),
        captured_(static_cast<std::size_t>(samples)) {
    valid_ = preload();
  }

  [[nodiscard]] bool valid() const noexcept {
    return valid_;
  }

  [[nodiscard]] std::uint64_t execute() noexcept {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t event = 0U; event < samples_; ++event) {
      auto& captured = captured_[static_cast<std::size_t>(event)];
      captured.result = book_.submit_limit(OrderId{samples_ + event + 1U}, Side::buy, Price{101},
                                           Quantity{1U}, TimeInForce::ioc, trade_buffer_);
      captured.trade = trade_buffer_.front();
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
  }

  [[nodiscard]] bool validate(std::uint64_t& checksum) const noexcept {
    checksum = kChecksumBasis;
    for (std::uint64_t event = 0U; event < samples_; ++event) {
      const auto& captured = captured_[static_cast<std::size_t>(event)];
      const OrderId taker{samples_ + event + 1U};
      const OrderId maker{event + 1U};
      if (captured.result.reject_reason != RejectReason::none ||
          captured.result.executed_quantity != Quantity{1U} ||
          captured.result.unfilled_quantity != Quantity{0U} || captured.result.trade_count != 1U ||
          captured.trade.buy_id != taker || captured.trade.sell_id != maker ||
          captured.trade.price != Price{101} || captured.trade.quantity != Quantity{1U}) {
        return false;
      }
      checksum_value(checksum, captured.trade.buy_id.value());
      checksum_value(checksum, captured.trade.sell_id.value());
      checksum_value(checksum, static_cast<std::uint64_t>(captured.trade.price.ticks()));
      checksum_value(checksum, captured.trade.quantity.value());
    }
    return true;
  }

private:
  [[nodiscard]] bool preload() noexcept {
    for (std::uint64_t event = 0U; event < samples_; ++event) {
      const auto result = book_.submit_limit(OrderId{event + 1U}, Side::sell, Price{101},
                                             Quantity{1U}, trade_buffer_);
      if (result.reject_reason != RejectReason::none || result.trade_count != 0U) {
        return false;
      }
    }
    return true;
  }

  std::uint64_t samples_;
  OrderBook book_;
  std::vector<Trade> trade_buffer_;
  std::vector<CapturedEvent> captured_;
  bool valid_{};
};

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t minimum,
                                  std::uint64_t maximum, std::uint64_t& value) noexcept {
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= minimum &&
         value <= maximum;
}

[[nodiscard]] bool parse_finite_double(std::string_view text, double minimum, double maximum,
                                       double& value) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] bool safe_output_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  return std::none_of(path.begin(), path.end(),
                      [](const auto& component) { return component == ".."; });
}

} // namespace

std::uint64_t median(std::vector<std::uint64_t> values) noexcept {
  if (values.empty()) {
    return 0U;
  }
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2U;
  if (values.size() % 2U != 0U) {
    return values[middle];
  }
  const auto lower = values[middle - 1U];
  const auto upper = values[middle];
  return lower + ((upper - lower) / 2U);
}

std::optional<Statistics> summarize_durations(std::vector<std::uint64_t> durations,
                                              std::uint64_t samples) {
  if (durations.empty() || samples == 0U || samples > kMaximumSamples ||
      std::any_of(durations.begin(), durations.end(),
                  [](const auto value) { return value == 0U; })) {
    return std::nullopt;
  }
  Statistics statistics{};
  statistics.samples = samples;
  statistics.repetitions = static_cast<std::uint64_t>(durations.size());
  statistics.elapsed_ns = std::move(durations);
  statistics.best_elapsed_ns =
      *std::min_element(statistics.elapsed_ns.begin(), statistics.elapsed_ns.end());
  statistics.median_elapsed_ns = median(statistics.elapsed_ns);
  if (statistics.median_elapsed_ns == 0U) {
    return std::nullopt;
  }
  std::vector<std::uint64_t> deviations;
  deviations.reserve(statistics.elapsed_ns.size());
  for (const auto duration : statistics.elapsed_ns) {
    deviations.push_back(duration > statistics.median_elapsed_ns
                             ? duration - statistics.median_elapsed_ns
                             : statistics.median_elapsed_ns - duration);
  }
  statistics.median_absolute_deviation_ns = median(std::move(deviations));
  statistics.relative_mad = static_cast<double>(statistics.median_absolute_deviation_ns) /
                            static_cast<double>(statistics.median_elapsed_ns);
  const long double operations = static_cast<long double>(samples);
  const long double throughput = operations * static_cast<long double>(kNanosecondsPerSecond) /
                                 static_cast<long double>(statistics.best_elapsed_ns);
  if (throughput > static_cast<long double>(std::numeric_limits<double>::max())) {
    return std::nullopt;
  }
  statistics.best_ops_per_second = static_cast<double>(throughput);
  if (!std::isfinite(statistics.relative_mad) || !std::isfinite(statistics.best_ops_per_second)) {
    return std::nullopt;
  }
  return statistics;
}

bool passes_gate(const Statistics& statistics, double minimum_ops_per_second,
                 double maximum_relative_mad) noexcept {
  return statistics.validation_passed && std::isfinite(statistics.best_ops_per_second) &&
         std::isfinite(statistics.relative_mad) && std::isfinite(minimum_ops_per_second) &&
         std::isfinite(maximum_relative_mad) &&
         statistics.best_ops_per_second >= minimum_ops_per_second &&
         statistics.relative_mad <= maximum_relative_mad;
}

std::optional<std::string> statistics_json(const Statistics& statistics,
                                           double minimum_ops_per_second,
                                           double maximum_relative_mad) {
  if (statistics.elapsed_ns.empty() || !std::isfinite(statistics.relative_mad) ||
      !std::isfinite(statistics.best_ops_per_second) || !std::isfinite(minimum_ops_per_second) ||
      !std::isfinite(maximum_relative_mad)) {
    return std::nullopt;
  }
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(17)
         << "{\"schema_version\":1,\"claim_scope\":\"ci_regression_only\","
            "\"metric\":\"batch_amortized_mean\",\"scenario\":\"crossing-limit\","
         << "\"samples\":" << statistics.samples << ",\"repetitions\":" << statistics.repetitions
         << ",\"elapsed_ns\":[";
  for (std::size_t index = 0U; index < statistics.elapsed_ns.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << statistics.elapsed_ns[index];
  }
  output << "],\"best_elapsed_ns\":" << statistics.best_elapsed_ns
         << ",\"median_elapsed_ns\":" << statistics.median_elapsed_ns
         << ",\"median_absolute_deviation_ns\":" << statistics.median_absolute_deviation_ns
         << ",\"relative_mad\":" << statistics.relative_mad
         << ",\"best_ops_per_second\":" << statistics.best_ops_per_second
         << ",\"minimum_ops_per_second\":" << minimum_ops_per_second
         << ",\"maximum_relative_mad\":" << maximum_relative_mad
         << ",\"validation_passed\":" << (statistics.validation_passed ? "true" : "false")
         << ",\"checksum\":" << statistics.checksum
         << ",\"gate_passed\":" << (statistics.gate_passed ? "true" : "false") << '}';
  return output.str();
}

std::optional<Config> parse_cli(const std::vector<std::string>& arguments, std::string* error) {
  Config config{};
  bool samples_set = false;
  bool repetitions_set = false;
  bool minimum_set = false;
  bool maximum_set = false;
  bool output_set = false;
  const auto fail = [&](std::string message) -> std::optional<Config> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  for (std::size_t index = 0U; index < arguments.size();) {
    const auto& option = arguments[index];
    if (option != "--samples" && option != "--repetitions" && option != "--min-ops-per-second" &&
        option != "--max-relative-mad" && option != "--output") {
      return fail("unknown or trailing argument: " + option);
    }
    if (index + 1U >= arguments.size()) {
      return fail("missing value for " + option);
    }
    const auto& value = arguments[index + 1U];
    if (option == "--samples") {
      if (samples_set || !parse_unsigned(value, 1U, kMaximumSamples, config.samples)) {
        return fail("invalid --samples");
      }
      samples_set = true;
    } else if (option == "--repetitions") {
      if (repetitions_set ||
          !parse_unsigned(value, kMinimumRepetitions, kMaximumRepetitions, config.repetitions)) {
        return fail("invalid --repetitions");
      }
      repetitions_set = true;
    } else if (option == "--min-ops-per-second") {
      if (minimum_set || !parse_finite_double(value, 0.0, 1.0e15, config.minimum_ops_per_second)) {
        return fail("invalid --min-ops-per-second");
      }
      minimum_set = true;
    } else if (option == "--max-relative-mad") {
      if (maximum_set || !parse_finite_double(value, 0.0, 10.0, config.maximum_relative_mad)) {
        return fail("invalid --max-relative-mad");
      }
      maximum_set = true;
    } else {
      if (output_set || value.empty()) {
        return fail("invalid --output");
      }
      config.output = std::filesystem::path{value};
      if (!safe_output_path(*config.output)) {
        return fail("invalid --output");
      }
      output_set = true;
    }
    index += 2U;
  }
  return config;
}

std::optional<Statistics> run(const Config& config, std::string& error) {
  if (config.samples == 0U || config.samples > kMaximumSamples ||
      config.repetitions < kMinimumRepetitions || config.repetitions > kMaximumRepetitions ||
      !std::isfinite(config.minimum_ops_per_second) ||
      !std::isfinite(config.maximum_relative_mad)) {
    error = "invalid throughput gate configuration";
    return std::nullopt;
  }
  std::vector<std::uint64_t> durations;
  durations.reserve(static_cast<std::size_t>(config.repetitions));
  bool validation_passed = true;
  std::optional<std::uint64_t> expected_checksum;
  for (std::uint64_t repetition = 0U; repetition < config.repetitions; ++repetition) {
    CrossingBatch batch{config.samples};
    if (!batch.valid()) {
      error = "crossing-limit preload failed";
      return std::nullopt;
    }
    durations.push_back(batch.execute());
    std::uint64_t checksum{};
    if (!batch.validate(checksum)) {
      validation_passed = false;
    }
    if (expected_checksum.has_value() && checksum != *expected_checksum) {
      validation_passed = false;
    }
    expected_checksum = checksum;
  }
  auto statistics = summarize_durations(std::move(durations), config.samples);
  if (!statistics.has_value()) {
    error = "invalid or unrepresentable duration sample";
    return std::nullopt;
  }
  statistics->validation_passed = validation_passed;
  statistics->checksum = expected_checksum.value_or(0U);
  statistics->gate_passed =
      passes_gate(*statistics, config.minimum_ops_per_second, config.maximum_relative_mad);
  return statistics;
}

} // namespace matching_engine::throughput_gate
