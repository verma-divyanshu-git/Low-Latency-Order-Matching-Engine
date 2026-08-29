#include "matching_engine/replay.hpp"
#include "matching_engine/snapshot.hpp"

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace matching_engine {
namespace {

struct Options {
  std::optional<std::string> journal;
  std::optional<std::string> snapshot;
  std::optional<std::int64_t> minimum;
  std::optional<std::int64_t> maximum;
  std::optional<std::uint64_t> tick;
  std::optional<std::uint64_t> max_orders;
  std::optional<std::uint64_t> max_quantity;
  std::optional<std::uint32_t> expect_crc;
  std::optional<std::uint64_t> expect_events;
  std::optional<std::uint64_t> expect_live_orders;
  bool help{};
};

template <typename Integer> bool parse_decimal(std::string_view text, Integer& value) noexcept {
  if (text.empty() || text.front() == '+' || (text.size() > 1U && text.front() == '0') ||
      (text.size() > 2U && text[0] == '-' && text[1] == '0')) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_hex_crc(std::string_view text, std::uint32_t& value) noexcept {
  if (text.size() != 8U) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

template <typename Value, typename Parser>
bool set_once(std::optional<Value>& destination, std::string_view option, std::string_view value,
              Parser parser, std::string& error) {
  if (destination.has_value()) {
    error = "duplicate " + std::string{option};
    return false;
  }
  Value parsed{};
  if (!parser(value, parsed)) {
    error = "invalid value for " + std::string{option};
    return false;
  }
  destination = std::move(parsed);
  return true;
}

std::expected<Options, std::string> parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help") {
      if (options.help) {
        return std::unexpected{"duplicate --help"};
      }
      options.help = true;
      continue;
    }
    const auto needs_value = [&](std::string_view name) { return option == name; };
    if (!(needs_value("--journal") || needs_value("--snapshot") || needs_value("--min") ||
          needs_value("--max") || needs_value("--tick") || needs_value("--max-orders") ||
          needs_value("--max-quantity") || needs_value("--expect-crc32c") ||
          needs_value("--expect-events") || needs_value("--expect-live-orders"))) {
      return std::unexpected{"unknown option " + std::string{option}};
    }
    if (index + 1 >= argc) {
      return std::unexpected{"missing value for " + std::string{option}};
    }
    const std::string_view value{argv[++index]};
    std::string error;
    if (option == "--journal" || option == "--snapshot") {
      auto& destination = option == "--journal" ? options.journal : options.snapshot;
      if (destination.has_value()) {
        return std::unexpected{"duplicate " + std::string{option}};
      }
      if (value.empty()) {
        return std::unexpected{"invalid value for " + std::string{option}};
      }
      destination = std::string{value};
      continue;
    }
    if (option == "--min") {
      if (!set_once(options.minimum, option, value, parse_decimal<std::int64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--max") {
      if (!set_once(options.maximum, option, value, parse_decimal<std::int64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--tick") {
      if (!set_once(options.tick, option, value, parse_decimal<std::uint64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--max-orders") {
      if (!set_once(options.max_orders, option, value, parse_decimal<std::uint64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--max-quantity") {
      if (!set_once(options.max_quantity, option, value, parse_decimal<std::uint64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--expect-crc32c") {
      if (!set_once(options.expect_crc, option, value, parse_hex_crc, error)) {
        return std::unexpected{error};
      }
    } else if (option == "--expect-events") {
      if (!set_once(options.expect_events, option, value, parse_decimal<std::uint64_t>, error)) {
        return std::unexpected{error};
      }
    } else if (!set_once(options.expect_live_orders, option, value, parse_decimal<std::uint64_t>,
                         error)) {
      return std::unexpected{error};
    }
  }
  return options;
}

int fail(std::string_view message) {
  std::cerr << "matching_engine_replay: " << message << '\n';
  return 2;
}

std::string nullable_price(std::optional<Price> price) {
  return price.has_value() ? std::to_string(price->ticks()) : "null";
}

std::string_view replay_error_message(ReplayError error) noexcept {
  switch (error) {
  case ReplayError::journal:
    return "journal read failed";
  case ReplayError::engine_state_mismatch:
    return "engine and snapshot boundary mismatch";
  case ReplayError::unverifiable_boundary:
    return "terminal snapshot boundary is unverifiable";
  case ReplayError::boundary_missing:
    return "snapshot boundary missing from journal";
  case ReplayError::boundary_time_mismatch:
    return "snapshot boundary logical time mismatch";
  case ReplayError::sequence_gap:
    return "journal suffix sequence mismatch";
  case ReplayError::apply:
    return "command apply failed";
  case ReplayError::invariant:
    return "engine invariant failure";
  }
  return "replay failed";
}

} // namespace
} // namespace matching_engine

int main(int argc, char** argv) {
  using namespace matching_engine;
  const auto parsed = parse_options(argc, argv);
  if (!parsed.has_value()) {
    return fail(parsed.error());
  }
  const Options& options = *parsed;
  if (options.help) {
    if (argc != 2) {
      return fail("--help cannot be combined with other options");
    }
    std::cout << "Usage: matching_engine_replay --journal PATH [--snapshot PATH | "
                 "--min N --max N --tick 1 --max-orders N --max-quantity N] "
                 "[--expect-crc32c XXXXXXXX] [--expect-events N] [--expect-live-orders N]\n";
    return 0;
  }
  if (!options.journal.has_value()) {
    return fail("missing --journal");
  }
  const bool any_config = options.minimum.has_value() || options.maximum.has_value() ||
                          options.tick.has_value() || options.max_orders.has_value() ||
                          options.max_quantity.has_value();
  std::unique_ptr<SequencedEngine> engine;
  SnapshotPoint point{};
  if (options.snapshot.has_value()) {
    if (any_config) {
      return fail("engine config conflicts with --snapshot");
    }
    auto loaded = load_snapshot(*options.snapshot);
    if (!loaded.has_value()) {
      return fail(snapshot_error_message(loaded.error()));
    }
    point = loaded->point;
    engine = std::move(loaded->engine);
  } else {
    if (!options.minimum.has_value() || !options.maximum.has_value() || !options.tick.has_value() ||
        !options.max_orders.has_value() || !options.max_quantity.has_value()) {
      return fail("missing engine config");
    }
    if (*options.tick != 1U) {
      return fail("--tick must be 1 for integer Price ticks");
    }
    if (*options.maximum < *options.minimum) {
      return fail("--max must be greater than or equal to --min");
    }
    const std::uint64_t distance =
        static_cast<std::uint64_t>(*options.maximum) - static_cast<std::uint64_t>(*options.minimum);
    if (distance >= kMaximumPriceLevels || *options.max_orders > kMaximumSnapshotSlots ||
        *options.max_quantity == 0U ||
        *options.max_quantity > std::numeric_limits<std::uint32_t>::max()) {
      return fail("engine config out of range");
    }
    try {
      engine = std::make_unique<SequencedEngine>(
          PriceDomain{Price{*options.minimum}, static_cast<std::uint32_t>(distance + 1U)},
          static_cast<std::size_t>(*options.max_orders), Quantity{*options.max_quantity});
    } catch (...) {
      return fail("invalid engine config");
    }
  }
  auto journal = MmapJournal::open(*options.journal);
  if (!journal.has_value()) {
    return fail(journal_error_message(journal.error().operation));
  }
  std::vector<EngineEvent> events(engine->maximum_event_capacity());
  const auto replayed =
      replay_journal(*journal, *engine, point.sequence, point.logical_time, events);
  if (!replayed.has_value()) {
    return fail(replay_error_message(replayed.error()));
  }
  const InvariantResult invariant = engine->order_book().check_invariants();
  if (invariant.violation != InvariantViolation::none) {
    return fail("engine invariant failure");
  }
  if (options.expect_crc.has_value() && replayed->fingerprint.crc32c() != *options.expect_crc) {
    return fail("expected CRC32C mismatch");
  }
  if (options.expect_events.has_value() &&
      replayed->fingerprint.event_count() != *options.expect_events) {
    return fail("expected event count mismatch");
  }
  if (options.expect_live_orders.has_value() &&
      invariant.reachable_count != *options.expect_live_orders) {
    return fail("expected live order count mismatch");
  }
  std::ostringstream crc;
  crc << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
      << replayed->fingerprint.crc32c();
  std::cout << "{\"format_version\":1,\"commands_applied\":" << replayed->commands_applied
            << ",\"first_sequence\":" << replayed->first_sequence.value()
            << ",\"last_sequence\":" << replayed->last_sequence.value()
            << ",\"event_count\":" << replayed->fingerprint.event_count()
            << ",\"event_bytes\":" << replayed->fingerprint.byte_count() << ",\"crc32c\":\""
            << crc.str() << "\",\"live_orders\":" << invariant.reachable_count
            << ",\"best_bid\":" << nullable_price(engine->order_book().best_bid())
            << ",\"best_ask\":" << nullable_price(engine->order_book().best_ask())
            << ",\"snapshot_sequence\":" << point.sequence.value() << "}\n";
  return 0;
}
