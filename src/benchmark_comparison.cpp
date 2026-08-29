#include "matching_engine/benchmark_comparison.hpp"

#include "matching_engine/hierarchical_bitmap.hpp"

#include "absl/container/btree_map.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace matching_engine::benchmark_comparison {
namespace {

constexpr std::uint64_t kChecksumBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kChecksumPrime = 1'099'511'628'211ULL;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;

void fold(std::uint64_t& checksum, std::uint64_t value) noexcept {
  checksum ^= value;
  checksum *= kChecksumPrime;
}

[[nodiscard]] std::uint64_t quantity(std::uint64_t key) noexcept {
  return (key % 97U) + 1U;
}

[[nodiscard]] std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2U;
  return values.size() % 2U == 0U
             ? values[middle - 1U] + ((values[middle] - values[middle - 1U]) / 2U)
             : values[middle];
}

class LadderBitmapWorkload {
public:
  LadderBitmapWorkload(std::uint32_t active, std::uint32_t operations)
      : active_{active}, operations_{operations}, bitmap_{active + operations},
        quantities_(static_cast<std::size_t>(active) + operations) {
    for (std::uint32_t key = 0U; key < active_; ++key) {
      static_cast<void>(bitmap_.set(key));
      quantities_[key] = quantity(key);
    }
  }

  [[nodiscard]] std::uint64_t execute() noexcept {
    std::uint64_t checksum = kChecksumBasis;
    for (std::uint32_t index = 0U; index < operations_; ++index) {
      const std::uint32_t key = *bitmap_.first_set();
      fold(checksum, key);
      fold(checksum, quantities_[key]);
      static_cast<void>(bitmap_.clear(key));
      const std::uint32_t inserted = active_ + index;
      quantities_[inserted] = quantity(inserted);
      static_cast<void>(bitmap_.set(inserted));
    }
    return checksum;
  }

private:
  std::uint32_t active_;
  std::uint32_t operations_;
  HierarchicalBitmap bitmap_;
  std::vector<std::uint64_t> quantities_;
};

template <typename Map>
class MapWorkload {
public:
  MapWorkload(std::uint32_t active, std::uint32_t operations)
      : active_{active}, operations_{operations} {
    for (std::uint32_t key = 0U; key < active_; ++key) {
      values_.emplace(key, quantity(key));
    }
  }

  [[nodiscard]] std::uint64_t execute() {
    std::uint64_t checksum = kChecksumBasis;
    for (std::uint32_t index = 0U; index < operations_; ++index) {
      const auto first = values_.begin();
      fold(checksum, first->first);
      fold(checksum, first->second);
      values_.erase(first);
      const std::uint32_t inserted = active_ + index;
      values_.emplace(inserted, quantity(inserted));
    }
    return checksum;
  }

private:
  std::uint32_t active_;
  std::uint32_t operations_;
  Map values_;
};

class SortedVectorWorkload {
public:
  SortedVectorWorkload(std::uint32_t active, std::uint32_t operations)
      : active_{active}, operations_{operations} {
    values_.reserve(active);
    for (std::uint32_t key = 0U; key < active_; ++key) {
      values_.emplace_back(key, quantity(key));
    }
  }

  [[nodiscard]] std::uint64_t execute() {
    std::uint64_t checksum = kChecksumBasis;
    for (std::uint32_t index = 0U; index < operations_; ++index) {
      fold(checksum, values_.front().first);
      fold(checksum, values_.front().second);
      values_.erase(values_.begin());
      const std::uint32_t inserted = active_ + index;
      values_.emplace_back(inserted, quantity(inserted));
    }
    return checksum;
  }

private:
  std::uint32_t active_;
  std::uint32_t operations_;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> values_;
};

template <typename Workload>
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> measure(Workload& workload) {
  const auto start = std::chrono::steady_clock::now();
  const std::uint64_t checksum = workload.execute();
  const auto finish = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
  return {elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U, checksum};
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t minimum,
                                  std::uint64_t maximum, std::uint64_t& output) noexcept {
  if (text.empty() || text.front() == '+' || text.front() == '-' ||
      (text.size() > 1U && text.front() == '0')) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output, 10);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         output >= minimum && output <= maximum;
}

[[nodiscard]] bool safe_path(const std::filesystem::path& path) {
  return !path.empty() && std::none_of(path.begin(), path.end(),
                                      [](const auto& part) { return part == ".."; });
}

} // namespace

const char* implementation_name(Implementation implementation) noexcept {
  switch (implementation) {
  case Implementation::ladder_bitmap: return "ladder_bitmap";
  case Implementation::standard_map: return "std_map";
  case Implementation::sorted_vector: return "sorted_vector";
  case Implementation::abseil_btree: return "absl_btree_map";
  }
  return "unknown";
}

std::optional<Config> parse_cli(const std::vector<std::string>& arguments, std::string* error) {
  Config config;
  bool active_set = false;
  bool operations_set = false;
  bool repetitions_set = false;
  const auto fail = [&](std::string message) -> std::optional<Config> {
    if (error != nullptr) *error = std::move(message);
    return std::nullopt;
  };
  for (std::size_t index = 0U; index < arguments.size(); ++index) {
    const std::string_view option = arguments[index];
    if (index + 1U == arguments.size()) return fail("missing option value");
    const std::string_view value = arguments[++index];
    std::uint64_t number{};
    if (option == "--active-levels") {
      if (active_set || !parse_unsigned(value, 1U, kMaximumComparisonOperations, number))
        return fail("invalid --active-levels");
      active_set = true;
      config.active_levels = number;
    } else if (option == "--operations") {
      if (operations_set || !parse_unsigned(value, 1U, kMaximumComparisonOperations, number))
        return fail("invalid --operations");
      operations_set = true;
      config.operations = number;
    } else if (option == "--repetitions") {
      if (repetitions_set || !parse_unsigned(value, 3U, 21U, number))
        return fail("invalid --repetitions");
      repetitions_set = true;
      config.repetitions = number;
    } else if (option == "--output") {
      if (config.output.has_value() || value.empty()) return fail("invalid --output");
      config.output = std::filesystem::path{value};
      if (!safe_path(*config.output)) return fail("invalid --output");
    } else {
      return fail("unknown option");
    }
  }
  if (config.active_levels > kMaximumComparisonOperations - config.operations)
    return fail("active levels plus operations exceed limit");
  return config;
}

std::optional<Report> run(const Config& config, std::string& error) {
  if (config.active_levels == 0U || config.operations == 0U || config.repetitions < 3U ||
      config.repetitions > 21U ||
      config.active_levels > kMaximumComparisonOperations - config.operations) {
    error = "invalid comparison configuration";
    return std::nullopt;
  }
  Report report{.config = config};
  const auto execute = [&]<typename Workload>(Implementation implementation) {
    Result result{.implementation = implementation};
    result.elapsed_ns.reserve(static_cast<std::size_t>(config.repetitions));
    for (std::uint64_t repetition = 0U; repetition < config.repetitions; ++repetition) {
      Workload workload{static_cast<std::uint32_t>(config.active_levels),
                        static_cast<std::uint32_t>(config.operations)};
      const auto [elapsed, checksum] = measure(workload);
      if (elapsed == 0U || (result.checksum != 0U && result.checksum != checksum)) return false;
      result.elapsed_ns.push_back(elapsed);
      result.checksum = checksum;
    }
    result.median_elapsed_ns = median(result.elapsed_ns);
    result.median_operations_per_second =
        static_cast<double>(config.operations) * static_cast<double>(kNanosecondsPerSecond) /
        static_cast<double>(result.median_elapsed_ns);
    report.results.push_back(std::move(result));
    return true;
  };
  if (!execute.template operator()<LadderBitmapWorkload>(Implementation::ladder_bitmap) ||
      !execute.template operator()<MapWorkload<std::map<std::uint32_t, std::uint64_t>>>(
          Implementation::standard_map) ||
      !execute.template operator()<SortedVectorWorkload>(Implementation::sorted_vector) ||
      !execute.template operator()<MapWorkload<absl::btree_map<std::uint32_t, std::uint64_t>>>(
          Implementation::abseil_btree)) {
    error = "comparison execution failed";
    return std::nullopt;
  }
  report.validation_passed = std::all_of(
      report.results.begin(), report.results.end(), [&](const Result& result) {
        return result.checksum == report.results.front().checksum &&
               std::isfinite(result.median_operations_per_second);
      });
  if (!report.validation_passed) error = "comparison checksum mismatch";
  return report.validation_passed ? std::optional{std::move(report)} : std::nullopt;
}

std::optional<std::string> report_json(const Report& report) {
  if (!report.validation_passed || report.results.size() != 4U) return std::nullopt;
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(17)
         << "{\"schema_version\":1,\"claim_scope\":\"regression_only\","
            "\"metric\":\"batch_amortized_mean\",\"workload\":\"pop_min_insert_next\","
         << "\"active_levels\":" << report.config.active_levels << ",\"operations\":"
         << report.config.operations << ",\"repetitions\":" << report.config.repetitions
         << ",\"validation_passed\":true,\"results\":[";
  for (std::size_t index = 0U; index < report.results.size(); ++index) {
    if (index != 0U) output << ',';
    const Result& result = report.results[index];
    output << "{\"implementation\":\"" << implementation_name(result.implementation)
           << "\",\"elapsed_ns\":[";
    for (std::size_t sample = 0U; sample < result.elapsed_ns.size(); ++sample) {
      if (sample != 0U) output << ',';
      output << result.elapsed_ns[sample];
    }
    output << "],\"median_elapsed_ns\":" << result.median_elapsed_ns
           << ",\"median_operations_per_second\":" << result.median_operations_per_second
           << ",\"checksum\":" << result.checksum << '}';
  }
  output << "]}";
  return output.str();
}

} // namespace matching_engine::benchmark_comparison
