#include "matching_engine/replay.hpp"
#include "matching_engine/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>

namespace matching_engine {
namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("matching-engine-snapshot-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
             "-" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_snapshot_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_snapshot_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_market_data_file(const std::filesystem::path& path,
                            std::span<const MarketDataMessage> messages) {
  std::ofstream output{path, std::ios::binary};
  for (const MarketDataMessage& message : messages) {
    std::array<std::byte, kEncodedMarketDataFrameSize> frame{};
    ASSERT_EQ(encode_market_data_frame(message, frame), MarketDataFrameError::none);
    output.write(reinterpret_cast<const char*>(frame.data()),
                 static_cast<std::streamsize>(frame.size()));
  }
}

template <typename Mutation>
void expect_snapshot_mutation(const std::vector<std::byte>& source, Mutation mutation,
                              SnapshotError expected) {
  auto bytes = source;
  mutation(bytes);
  rewrite_snapshot_crc_for_testing(bytes);
  const auto decoded = decode_snapshot(bytes);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), expected);
}

TEST(EngineEventCodecTest, HasCanonicalLittleEndianGoldenVector) {
  const EngineEvent event{.command_sequence = Sequence{0x0102030405060708ULL},
                          .order_id = OrderId{0x1112131415161718ULL},
                          .secondary_order_id = OrderId{0x2122232425262728ULL},
                          .price = Price{-2},
                          .quantity = Quantity{0x3132333435363738ULL},
                          .secondary_quantity = Quantity{0x4142434445464748ULL},
                          .handle = Handle{0x51525354U, 0x61626364U},
                          .event_index = 0x71727374U,
                          .type = EngineEventType::amend_result,
                          .reason = 0x81U};
  std::array<std::byte, kEncodedEngineEventSize> bytes{};
  ASSERT_EQ(encode_engine_event(event, bytes), EventCodecError::none);
  EXPECT_EQ(bytes[0], std::byte{0x08});
  EXPECT_EQ(bytes[7], std::byte{0x01});
  EXPECT_EQ(bytes[24], std::byte{0xfe});
  EXPECT_EQ(bytes[31], std::byte{0xff});
  EXPECT_EQ(bytes[56], std::byte{0x74});
  EXPECT_EQ(bytes[60], std::byte{0x03});
  EXPECT_EQ(bytes[61], std::byte{0x81});
  EXPECT_EQ(bytes[62], std::byte{0x00});
  EXPECT_EQ(bytes[63], std::byte{0x00});
  EXPECT_EQ(decode_engine_event(bytes), event);
}

TEST(ReplayFingerprintTest, StreamsCanonicalEventBytes) {
  const EngineEvent event{.command_sequence = Sequence{1U},
                          .order_id = OrderId{2U},
                          .price = Price{3},
                          .quantity = Quantity{4U},
                          .handle = Handle{5U, 6U},
                          .type = EngineEventType::submit_result};
  ReplayFingerprint fingerprint;
  ASSERT_EQ(fingerprint.add(event), EventCodecError::none);
  EXPECT_EQ(fingerprint.event_count(), 1U);
  EXPECT_EQ(fingerprint.byte_count(), kEncodedEngineEventSize);
  EXPECT_EQ(fingerprint.crc32c(), 0x48a49a71U);
}

TEST(MarketDataReplayTest, ReplaysValidatedFramesThroughGatewayAndMatcher) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "market-data.bin";
  const std::array<MarketDataMessage, 3U> messages{
      MarketDataMessage{.sequence = 1U,
                        .order_id = OrderId{1U},
                        .price = Price{101},
                        .quantity = Quantity{2U},
                        .type = MarketDataMessageType::add_order,
                        .side = Side::sell},
      MarketDataMessage{.sequence = 2U,
                        .order_id = OrderId{2U},
                        .price = Price{101},
                        .quantity = Quantity{2U},
                        .type = MarketDataMessageType::add_order,
                        .side = Side::buy},
      MarketDataMessage{.sequence = 3U,
                        .order_id = OrderId{1U},
                        .type = MarketDataMessageType::delete_order}};
  write_market_data_file(path, messages);
  auto input = MarketDataInputStream::open(path);
  ASSERT_TRUE(input.has_value());
  GatewayConfig config{.max_active_orders = 4U,
                       .max_lanes = 1U,
                       .max_quantity = Quantity{10U},
                       .max_notional = 1'000U,
                       .min_price = Price{100},
                       .max_price = Price{102},
                       .max_orders_per_second = 4U};
  MarketDataAdapter adapter{GatewayValidator{config}};
  SequencedEngine engine{PriceDomain{Price{100}, 3U}, 4U, Quantity{10U}};
  std::array<EngineEvent, 5U> events{};

  const auto replayed = replay_market_data(*input, adapter, engine, events);
  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 3U);
  EXPECT_EQ(replayed->first_sequence, Sequence{1U});
  EXPECT_EQ(replayed->last_sequence, Sequence{3U});
  EXPECT_EQ(replayed->fingerprint.event_count(), 4U);
  EXPECT_EQ(engine.order_book().best_bid(), std::nullopt);
  EXPECT_EQ(engine.order_book().best_ask(), std::nullopt);
}

TEST(SnapshotCodecTest, EmptyStateHasStableSizeAndRoundTrips) {
  SequencedEngine engine{PriceDomain{Price{-4}, 9U}, 3U, Quantity{100U}};
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded->size(), kSnapshotHeaderSize + 3U * kSnapshotSlotSize);

  auto restored = decode_snapshot(*encoded);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->point, (SnapshotPoint{Sequence{0U}, 0U}));
  EXPECT_EQ(restored->engine->order_book().check_invariants().violation, InvariantViolation::none);
  EXPECT_EQ(restored->engine->order_book().best_bid(), std::nullopt);
}

TEST(SnapshotCodecTest, PreservesGenerationAndFifoState) {
  SequencedEngine engine{PriceDomain{Price{100}, 5U}, 3U, Quantity{100U}};
  std::array<EngineEvent, 4U> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{101}, Quantity{5U}),
                  Sequence{1U}, 10U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle canceled = events[0].handle;
  ASSERT_EQ(engine.apply({CommandPayload::cancel(canceled), Sequence{2U}, 11U}, events).status,
            ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{103}, Quantity{7U}),
                  Sequence{3U}, 12U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle live = events[0].handle;

  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{3U}, 12U});
  ASSERT_TRUE(encoded.has_value());
  auto restored = decode_snapshot(*encoded);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->engine->order_book().order_info(live),
            (OrderInfo{OrderId{2U}, Side::sell, Price{103}, Quantity{7U}}));
  EXPECT_FALSE(restored->engine->order_book().order_info(canceled).has_value());
  EXPECT_EQ(restored->engine->order_book().best_ask(), Price{103});

  ASSERT_EQ(
      restored->engine->apply({CommandPayload::cancel(live), Sequence{4U}, 13U}, events).status,
      ApplyStatus::applied);
  ASSERT_EQ(
      restored->engine
          ->apply({CommandPayload::submit_limit(OrderId{3U}, Side::buy, Price{100}, Quantity{1U}),
                   Sequence{5U}, 14U},
                  events)
          .status,
      ApplyStatus::applied);
  EXPECT_EQ(events[0].handle.index, live.index);
  EXPECT_GT(events[0].handle.generation, live.generation);
}

TEST(SnapshotCodecTest, PreservesSelfTradePolicyAndTraderIdentity) {
  SequencedEngine engine{PriceDomain{Price{100}, 3U}, 2U, Quantity{10U}, Sequence{1U}, 0U,
                         SelfTradePolicy::cancel_taker};
  std::array<Trade, 2U> trades{};
  ASSERT_EQ(engine.order_book()
                .submit_limit(OrderId{1U}, TraderId{7U}, Side::sell, Price{101}, Quantity{4U}, trades)
                .reject_reason,
            RejectReason::none);
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(encoded.has_value());
  const auto restored = decode_snapshot(*encoded);
  ASSERT_TRUE(restored.has_value());

  std::array<Trade, 2U> restored_trades{};
  const SubmitResult blocked = restored->engine->order_book().submit_limit(
      OrderId{2U}, TraderId{7U}, Side::buy, Price{101}, Quantity{3U}, restored_trades);
  EXPECT_EQ(blocked.reject_reason, RejectReason::self_trade_prevented);
  EXPECT_EQ(restored->engine->order_book().level_info(Side::sell, Price{101}),
            (LevelInfo{Quantity{4U}, 1U}));
}

TEST(SnapshotCodecTest, RejectsCorruptionAndNoncanonicalDeadPayload) {
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 1U, Quantity{1U}};
  auto bytes = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(bytes.has_value());
  (*bytes)[8] = std::byte{3U};
  EXPECT_EQ(decode_snapshot(*bytes).error(), SnapshotError::unsupported_version);

  bytes = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(bytes.has_value());
  (*bytes)[kSnapshotHeaderSize + 12U] = std::byte{1U};
  rewrite_snapshot_crc_for_testing(*bytes);
  EXPECT_EQ(decode_snapshot(*bytes).error(), SnapshotError::noncanonical_slot);
}

TEST(SnapshotCodecTest, ReachesEveryHeaderAndArenaValidatorWithValidCrc) {
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 2U, Quantity{10U}};
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(encoded.has_value());
  const std::size_t first = kSnapshotHeaderSize;
  const std::size_t second = first + kSnapshotSlotSize;

  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 8U, 3U); },
      SnapshotError::unsupported_version);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { bytes[0U] = std::byte{'X'}; }, SnapshotError::invalid_header);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 12U, 111U); },
      SnapshotError::invalid_header);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 16U, 47U); },
      SnapshotError::invalid_header);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 20U, 2U); },
      SnapshotError::invalid_header);
  expect_snapshot_mutation(
      *encoded,
      [](auto& bytes) {
        write_snapshot_u64(bytes, 24U, static_cast<std::uint64_t>(bytes.size() + 1U));
      },
      SnapshotError::invalid_length);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { bytes[100U] = std::byte{1U}; }, SnapshotError::invalid_header);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 48U, 3U); },
      SnapshotError::invalid_length);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 44U, 0U); },
      SnapshotError::invalid_configuration);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 40U, kMaximumPriceLevels + 1U); },
      SnapshotError::price_level_limit);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 52U, 1U); },
      SnapshotError::live_count_mismatch);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u64(bytes, 72U, 1U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u64(bytes, 64U, 2U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u64(bytes, 80U, 1U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u64(bytes, 88U, 1U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 60U, 1U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 60U, 2U); },
      SnapshotError::invalid_sequence_state);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { bytes[first + 8U] = std::byte{2U}; },
      SnapshotError::noncanonical_slot);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { bytes[first + 9U] = std::byte{1U}; },
      SnapshotError::noncanonical_slot);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { bytes[first + 12U] = std::byte{1U}; },
      SnapshotError::noncanonical_slot);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { bytes[first + 44U] = std::byte{1U}; },
      SnapshotError::noncanonical_slot);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u32(bytes, first, 0U); },
      SnapshotError::invalid_slot_metadata);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u32(bytes, first + 4U, 2U); },
      SnapshotError::invalid_free_list);
  expect_snapshot_mutation(
      *encoded, [second](auto& bytes) { write_snapshot_u32(bytes, second + 4U, 0U); },
      SnapshotError::invalid_free_list);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 56U, 1U); },
      SnapshotError::invalid_free_list);

  auto checksum_failure = *encoded;
  checksum_failure[32U] ^= std::byte{1U};
  EXPECT_EQ(decode_snapshot(checksum_failure).error(), SnapshotError::checksum_mismatch);
}

TEST(SnapshotCodecTest, ReachesLiveOrderGraphAndCrossedBookValidatorsWithValidCrc) {
  SequencedEngine engine{PriceDomain{Price{100}, 4U}, 3U, Quantity{10U}};
  std::array<EngineEvent, 4U> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{100}, Quantity{2U}),
                  Sequence{1U}, 1U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{2U}, Side::buy, Price{100}, Quantity{3U}),
                  Sequence{2U}, 2U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{3U}, Side::sell, Price{103}, Quantity{1U}),
                  Sequence{3U}, 3U},
                 events)
          .status,
      ApplyStatus::applied);
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{3U}, 3U});
  ASSERT_TRUE(encoded.has_value());
  const std::size_t first = kSnapshotHeaderSize;
  const std::size_t second = first + kSnapshotSlotSize;

  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u64(bytes, first + 20U, 0U); },
      SnapshotError::invalid_order);
  expect_snapshot_mutation(
      *encoded,
      [first](auto& bytes) { write_snapshot_u32(bytes, first + 36U, kMaximumPriceLevels); },
      SnapshotError::invalid_order);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u32(bytes, first + 40U, 1U); },
      SnapshotError::invalid_order);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u32(bytes, first + 32U, 99U); },
      SnapshotError::invalid_order_graph);
  expect_snapshot_mutation(
      *encoded, [second](auto& bytes) { write_snapshot_u32(bytes, second + 28U, kInvalidIndex); },
      SnapshotError::invalid_order_graph);
  expect_snapshot_mutation(
      *encoded,
      [second](auto& bytes) {
        write_snapshot_u32(bytes, second + 36U, detail::encode_level_side(0U, Side::sell));
      },
      SnapshotError::invalid_order_graph);
  expect_snapshot_mutation(
      *encoded, [first](auto& bytes) { write_snapshot_u32(bytes, first + 32U, 0U); },
      SnapshotError::invalid_order_graph);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 52U, 2U); },
      SnapshotError::live_count_mismatch);
  expect_snapshot_mutation(
      *encoded, [](auto& bytes) { write_snapshot_u32(bytes, 56U, 0U); },
      SnapshotError::invalid_free_list);
  expect_snapshot_mutation(
      *encoded,
      [first, second](auto& bytes) {
        write_snapshot_u32(bytes, first + 36U, detail::encode_level_side(3U, Side::buy));
        write_snapshot_u32(bytes, second + 36U, detail::encode_level_side(3U, Side::buy));
      },
      SnapshotError::crossed_book);
}

TEST(SnapshotCodecTest, RejectsTruncationAndOversizedPriceDomainBeforeAllocation) {
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  const auto encoded = encode_snapshot(engine, SnapshotPoint{Sequence{0U}, 0U});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(
      decode_snapshot(std::span<const std::byte>{*encoded}.first(encoded->size() - 1U)).error(),
      SnapshotError::invalid_length);

  auto oversized = *encoded;
  write_snapshot_u32(oversized, 40U, kMaximumPriceLevels + 1U);
  rewrite_snapshot_crc_for_testing(oversized);
  EXPECT_EQ(decode_snapshot(oversized).error(), SnapshotError::price_level_limit);
}

TEST(SnapshotPersistenceTest, AtomicallyOverwritesAndLoadsMode0600File) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine first{PriceDomain{Price{10}, 2U}, 1U, Quantity{5U}};
  ASSERT_EQ(save_snapshot_atomic(path, first, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  SequencedEngine second{PriceDomain{Price{20}, 3U}, 2U, Quantity{9U}};
  std::array<EngineEvent, 3U> events{};
  ASSERT_EQ(
      second
          .apply({CommandPayload::submit_limit(OrderId{7U}, Side::buy, Price{21}, Quantity{4U}),
                  Sequence{1U}, 5U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(save_snapshot_atomic(path, second, SnapshotPoint{Sequence{1U}, 5U}),
            SnapshotError::none);

  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->point, (SnapshotPoint{Sequence{1U}, 5U}));
  EXPECT_EQ(loaded->engine->order_book().best_bid(), Price{21});
  EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

TEST(SnapshotPersistenceTest, RejectsSymlinkAndWrongMode) {
  TemporaryDirectory temporary;
  const auto real = temporary.path() / "real.snapshot";
  const auto link = temporary.path() / "link.snapshot";
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  ASSERT_EQ(save_snapshot_atomic(real, engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  std::filesystem::create_symlink(real.filename(), link);
  EXPECT_EQ(load_snapshot(link).error(), SnapshotError::symlink);
  std::filesystem::permissions(real, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add);
  EXPECT_EQ(load_snapshot(real).error(), SnapshotError::permission_denied);
}

TEST(SnapshotPersistenceTest, PreRenameFailureKeepsOldSnapshotAndCleansTemp) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine old_engine{PriceDomain{Price{10}, 1U}, 0U, Quantity{1U}};
  ASSERT_EQ(save_snapshot_atomic(path, old_engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  SequencedEngine replacement{PriceDomain{Price{20}, 1U}, 0U, Quantity{1U}};
  snapshot_testing::fail_for_path(
      path, snapshot_testing::failure_mask(snapshot_testing::FailurePoint::rename));

  EXPECT_EQ(save_snapshot_atomic(path, replacement, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::io_error);
  auto loaded = load_snapshot(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->engine->order_book().level_info(Side::buy, Price{10})->order_count, 0U);
  std::size_t entries{};
  for (const auto& ignored : std::filesystem::directory_iterator(temporary.path())) {
    static_cast<void>(ignored);
    ++entries;
  }
  EXPECT_EQ(entries, 1U);
}

TEST(SnapshotPersistenceTest, PostRenameDirectorySyncFailureIsIndeterminate) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine engine{PriceDomain{Price{10}, 1U}, 0U, Quantity{1U}};
  snapshot_testing::fail_for_path(
      path, snapshot_testing::failure_mask(snapshot_testing::FailurePoint::parent_fsync));

  EXPECT_EQ(save_snapshot_atomic(path, engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::commit_indeterminate);
  EXPECT_TRUE(load_snapshot(path).has_value());
}

TEST(SnapshotPersistenceTest, RejectsOversizedFileBeforeAllocation) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "engine.snapshot";
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  ASSERT_EQ(save_snapshot_atomic(path, engine, SnapshotPoint{Sequence{0U}, 0U}),
            SnapshotError::none);
  std::filesystem::resize_file(path, kMaximumSnapshotBytes + 1U);

  EXPECT_EQ(load_snapshot(path).error(), SnapshotError::file_too_large);
}

TEST(ReplayTest, SnapshotBoundarySkipsPrefixAndReplaysExactSuffix) {
  TemporaryDirectory temporary;
  const auto journal_path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(journal_path, 4U);
  ASSERT_TRUE(journal.has_value());
  const std::array commands{
      SequencedCommand{
          CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{101}, Quantity{5U}),
          Sequence{1U}, 10U},
      SequencedCommand{
          CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{103}, Quantity{4U}),
          Sequence{2U}, 11U},
      SequencedCommand{CommandPayload::submit_market(OrderId{3U}, Side::buy, Quantity{2U}),
                       Sequence{3U}, 12U},
      SequencedCommand{CommandPayload::amend_quantity(Handle{0U, 1U}, Quantity{3U}), Sequence{4U},
                       13U},
  };
  SequencedEngine uninterrupted{PriceDomain{Price{100}, 5U}, 4U, Quantity{10U}};
  std::array<EngineEvent, 5U> events{};
  for (std::size_t index = 0; index < 2U; ++index) {
    ASSERT_EQ(journal->append(commands[index]), JournalError::none);
    ASSERT_EQ(uninterrupted.apply(commands[index], events).status, ApplyStatus::applied);
  }
  const auto snapshot = encode_snapshot(uninterrupted, SnapshotPoint{Sequence{2U}, 11U});
  ASSERT_TRUE(snapshot.has_value());
  ReplayFingerprint expected;
  for (std::size_t index = 2U; index < commands.size(); ++index) {
    ASSERT_EQ(journal->append(commands[index]), JournalError::none);
    const ApplyResult applied = uninterrupted.apply(commands[index], events);
    ASSERT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t event_index = 0; event_index < applied.event_count; ++event_index) {
      ASSERT_EQ(expected.add(events[event_index]), EventCodecError::none);
    }
  }
  auto restored = decode_snapshot(*snapshot);
  ASSERT_TRUE(restored.has_value());
  auto replayed = replay_journal(*journal, *restored->engine, restored->point.sequence,
                                 restored->point.logical_time, events);
  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 2U);
  EXPECT_EQ(replayed->fingerprint.event_count(), expected.event_count());
  EXPECT_EQ(replayed->fingerprint.crc32c(), expected.crc32c());
  EXPECT_EQ(restored->engine->order_book().best_bid(), uninterrupted.order_book().best_bid());
  EXPECT_EQ(restored->engine->order_book().best_ask(), uninterrupted.order_book().best_ask());
}

TEST(ReplayTest, ExactMixedContinuationPreservesEventsHandlesAndNextAllocation) {
  TemporaryDirectory temporary;
  const auto journal_path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(journal_path, 14U);
  ASSERT_TRUE(journal.has_value());
  SequencedEngine uninterrupted{PriceDomain{Price{100}, 5U}, 4U, Quantity{20U}};
  std::array<EngineEvent, 6U> events{};
  std::uint64_t sequence = 1U;
  auto append_apply = [&](CommandPayload payload) {
    const SequencedCommand command{payload, Sequence{sequence}, sequence + 10U};
    ++sequence;
    EXPECT_EQ(journal->append(command), JournalError::none);
    const ApplyResult result = uninterrupted.apply(command, events);
    EXPECT_EQ(result.status, ApplyStatus::applied);
    return std::pair{command, result};
  };

  const auto first =
      append_apply(CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{101}, Quantity{5U}));
  ASSERT_EQ(first.second.event_count, 1U);
  const Handle canceled_after_snapshot = events[0].handle;
  const auto second =
      append_apply(CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{104}, Quantity{5U}));
  ASSERT_EQ(second.second.event_count, 1U);
  const Handle replaced_after_snapshot = events[0].handle;
  append_apply(CommandPayload::submit_limit(OrderId{3U}, Side::buy, Price{99}, Quantity{1U}));
  append_apply(CommandPayload::submit_limit(OrderId{4U}, Side::buy, Price{104}, Quantity{6U},
                                            TimeInForce::fok));
  append_apply(CommandPayload::submit_market(OrderId{5U}, Side::buy, Quantity{2U}));

  const auto snapshot = encode_snapshot(uninterrupted, SnapshotPoint{Sequence{5U}, 15U});
  ASSERT_TRUE(snapshot.has_value());
  auto exact_restored = decode_snapshot(*snapshot);
  auto helper_restored = decode_snapshot(*snapshot);
  ASSERT_TRUE(exact_restored.has_value());
  ASSERT_TRUE(helper_restored.has_value());

  std::vector<SequencedCommand> suffix;
  std::vector<EngineEvent> expected_events;
  ReplayFingerprint expected_fingerprint;
  auto append_suffix = [&](CommandPayload payload) {
    const auto [command, result] = append_apply(payload);
    suffix.push_back(command);
    for (std::size_t index = 0U; index < result.event_count; ++index) {
      expected_events.push_back(events[index]);
      ASSERT_EQ(expected_fingerprint.add(events[index]), EventCodecError::none);
    }
  };
  append_suffix(CommandPayload::cancel(canceled_after_snapshot));
  append_suffix(CommandPayload::amend_quantity(replaced_after_snapshot, Quantity{2U}));
  append_suffix(CommandPayload::replace(replaced_after_snapshot, Price{103}, Quantity{4U}));
  const Handle replacement = events[0].handle;
  append_suffix(CommandPayload::submit_market(OrderId{6U}, Side::buy, Quantity{1U}));
  append_suffix(CommandPayload::submit_limit(OrderId{7U}, Side::sell, Price{100}, Quantity{10U},
                                             TimeInForce::fok));
  append_suffix(CommandPayload::submit_limit(OrderId{8U}, Side::buy, Price{100}, Quantity{2U}));
  const Handle reused_canceled_slot = events[0].handle;
  append_suffix(CommandPayload::cancel(replaced_after_snapshot));
  append_suffix(CommandPayload::amend_quantity(replacement, Quantity{2U}));
  ASSERT_EQ(journal->close(), JournalError::none);

  std::vector<EngineEvent> actual_events;
  for (const SequencedCommand& command : suffix) {
    const ApplyResult applied = exact_restored->engine->apply(command, events);
    ASSERT_EQ(applied.status, ApplyStatus::applied);
    for (std::size_t index = 0U; index < applied.event_count; ++index) {
      actual_events.push_back(events[index]);
    }
  }
  ASSERT_EQ(actual_events, expected_events);
  for (std::size_t index = 0U; index < actual_events.size(); ++index) {
    std::array<std::byte, kEncodedEngineEventSize> actual_bytes{};
    std::array<std::byte, kEncodedEngineEventSize> expected_bytes{};
    ASSERT_EQ(encode_engine_event(actual_events[index], actual_bytes), EventCodecError::none);
    ASSERT_EQ(encode_engine_event(expected_events[index], expected_bytes), EventCodecError::none);
    EXPECT_EQ(actual_bytes, expected_bytes);
  }

  auto reopened = MmapJournal::open(journal_path);
  ASSERT_TRUE(reopened.has_value());
  const auto replayed =
      replay_journal(*reopened, *helper_restored->engine, Sequence{5U}, 15U, events);
  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, suffix.size());
  EXPECT_EQ(replayed->fingerprint.event_count(), expected_fingerprint.event_count());
  EXPECT_EQ(replayed->fingerprint.byte_count(), expected_fingerprint.byte_count());
  EXPECT_EQ(replayed->fingerprint.crc32c(), expected_fingerprint.crc32c());

  OrderBook& expected_book = uninterrupted.order_book();
  OrderBook& actual_book = helper_restored->engine->order_book();
  EXPECT_EQ(actual_book.order_info(canceled_after_snapshot),
            expected_book.order_info(canceled_after_snapshot));
  EXPECT_EQ(actual_book.order_info(replaced_after_snapshot),
            expected_book.order_info(replaced_after_snapshot));
  EXPECT_EQ(actual_book.order_info(replacement), expected_book.order_info(replacement));
  EXPECT_EQ(actual_book.order_info(reused_canceled_slot),
            expected_book.order_info(reused_canceled_slot));
  EXPECT_EQ(reused_canceled_slot.index, canceled_after_snapshot.index);
  EXPECT_GT(reused_canceled_slot.generation, canceled_after_snapshot.generation);
  EXPECT_FALSE(actual_book.order_info(canceled_after_snapshot).has_value());
  EXPECT_FALSE(actual_book.order_info(replaced_after_snapshot).has_value());
  EXPECT_TRUE(actual_book.order_info(replacement).has_value());
  EXPECT_TRUE(actual_book.order_info(reused_canceled_slot).has_value());
  EXPECT_EQ(actual_book.best_bid(), expected_book.best_bid());
  EXPECT_EQ(actual_book.best_ask(), expected_book.best_ask());
  for (Price price : {Price{100}, Price{101}, Price{103}, Price{104}}) {
    EXPECT_EQ(actual_book.level_info(Side::buy, price), expected_book.level_info(Side::buy, price));
    EXPECT_EQ(actual_book.level_info(Side::sell, price),
              expected_book.level_info(Side::sell, price));
  }
  EXPECT_EQ(actual_book.check_invariants(), expected_book.check_invariants());
  EXPECT_EQ(helper_restored->engine->next_sequence(), uninterrupted.next_sequence());
  EXPECT_EQ(helper_restored->engine->last_logical_time(), uninterrupted.last_logical_time());
  EXPECT_EQ(helper_restored->engine->sequence_exhausted(), uninterrupted.sequence_exhausted());

  const SequencedCommand next{
      CommandPayload::submit_limit(OrderId{9U}, Side::sell, Price{104}, Quantity{1U}),
      uninterrupted.next_sequence(), uninterrupted.last_logical_time() + 1U};
  const ApplyResult expected_next = uninterrupted.apply(next, events);
  ASSERT_EQ(expected_next.status, ApplyStatus::applied);
  std::vector<EngineEvent> expected_next_events(events.begin(),
                                                events.begin() + expected_next.event_count);
  const ApplyResult actual_next = helper_restored->engine->apply(next, events);
  ASSERT_EQ(actual_next, expected_next);
  EXPECT_EQ(std::vector<EngineEvent>(events.begin(), events.begin() + actual_next.event_count),
            expected_next_events);
}

TEST(ReplayBoundaryTest, AcceptsEmptySnapshotWithEmptyJournal) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  std::array<EngineEvent, 1U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{0U}, 0U, events);

  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 0U);
  EXPECT_EQ(engine.next_sequence(), Sequence{1U});
  EXPECT_EQ(engine.last_logical_time(), 0U);
  EXPECT_FALSE(engine.sequence_exhausted());
}

TEST(ReplayBoundaryTest, EmptySnapshotAppliesJournalCommands) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  ASSERT_EQ(
      journal->append({CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{0}, Quantity{1U}),
                       Sequence{1U}, 2U}),
      JournalError::none);
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 1U, Quantity{1U}};
  std::array<EngineEvent, 2U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{0U}, 0U, events);

  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 1U);
  EXPECT_EQ(engine.next_sequence(), Sequence{2U});
  EXPECT_EQ(engine.last_logical_time(), 2U);
}

TEST(ReplayBoundaryTest, SnapshotAtJournalEndAppliesNoSuffix) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  const SequencedCommand command{
      CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}), Sequence{1U}, 3U};
  ASSERT_EQ(journal->append(command), JournalError::none);
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}};
  std::array<EngineEvent, 1U> events{};
  ASSERT_EQ(engine.apply(command, events).status, ApplyStatus::applied);

  const auto replayed = replay_journal(*journal, engine, Sequence{1U}, 3U, events);

  ASSERT_TRUE(replayed.has_value());
  EXPECT_EQ(replayed->commands_applied, 0U);
  EXPECT_EQ(engine.next_sequence(), Sequence{2U});
}

TEST(ReplayBoundaryTest, RejectsTooShortJournalBeforeMutation) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}, Sequence{2U}, 7U};
  std::array<EngineEvent, 1U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{1U}, 7U, events);

  EXPECT_EQ(replayed.error(), ReplayError::boundary_missing);
  EXPECT_EQ(engine.next_sequence(), Sequence{2U});
  EXPECT_EQ(engine.last_logical_time(), 7U);
  EXPECT_EQ(engine.order_book().check_invariants().reachable_count, 0U);
}

TEST(ReplayBoundaryTest, RejectsBoundaryTimeMismatchBeforeMutation) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  ASSERT_EQ(journal->append({CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}),
                             Sequence{1U}, 8U}),
            JournalError::none);
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}, Sequence{2U}, 7U};
  std::array<EngineEvent, 1U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{1U}, 7U, events);

  EXPECT_EQ(replayed.error(), ReplayError::boundary_time_mismatch);
  EXPECT_EQ(engine.next_sequence(), Sequence{2U});
  EXPECT_EQ(engine.last_logical_time(), 7U);
}

TEST(ReplayBoundaryTest, RejectsMismatchedFreshEngineBeforeSuffixMutation) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  ASSERT_EQ(
      journal->append({CommandPayload::submit_limit(OrderId{1U}, Side::buy, Price{0}, Quantity{1U}),
                       Sequence{1U}, 1U}),
      JournalError::none);
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 1U, Quantity{1U}, Sequence{2U}, 0U};
  std::array<EngineEvent, 2U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{0U}, 0U, events);

  EXPECT_EQ(replayed.error(), ReplayError::engine_state_mismatch);
  EXPECT_EQ(engine.next_sequence(), Sequence{2U});
  EXPECT_EQ(engine.order_book().best_bid(), std::nullopt);
}

TEST(ReplayBoundaryTest, RejectsNonzeroLogicalTimeForSequenceZeroSnapshot) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U}, Sequence{1U}, 1U};
  std::array<EngineEvent, 1U> events{};

  const auto replayed = replay_journal(*journal, engine, Sequence{0U}, 1U, events);

  EXPECT_EQ(replayed.error(), ReplayError::engine_state_mismatch);
  EXPECT_EQ(engine.next_sequence(), Sequence{1U});
  EXPECT_EQ(engine.last_logical_time(), 1U);
}

TEST(ReplayBoundaryTest, RejectsTerminalSnapshotAsUnverifiableWithoutMutation) {
  TemporaryDirectory temporary;
  auto journal = MmapJournal::create(temporary.path() / "commands.journal", 1U);
  ASSERT_TRUE(journal.has_value());
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U},
                         Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U};
  std::array<EngineEvent, 1U> events{};
  ASSERT_EQ(engine
                .apply({CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}),
                        Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U},
                       events)
                .status,
            ApplyStatus::applied);
  ASSERT_TRUE(engine.sequence_exhausted());

  const auto replayed = replay_journal(
      *journal, engine, Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U, events);

  EXPECT_EQ(replayed.error(), ReplayError::unverifiable_boundary);
  EXPECT_TRUE(engine.sequence_exhausted());
  EXPECT_EQ(engine.last_logical_time(), 9U);
}

TEST(ReplayBoundaryTest, SnapshotCodecPreservesTerminalSequencerState) {
  SequencedEngine engine{PriceDomain{Price{0}, 1U}, 0U, Quantity{1U},
                         Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U};
  std::array<EngineEvent, 1U> events{};
  ASSERT_EQ(engine
                .apply({CommandPayload::submit_market(OrderId{1U}, Side::buy, Quantity{1U}),
                        Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U},
                       events)
                .status,
            ApplyStatus::applied);
  const auto bytes = encode_snapshot(
      engine, SnapshotPoint{Sequence{std::numeric_limits<std::uint64_t>::max()}, 9U});
  ASSERT_TRUE(bytes.has_value());

  const auto restored = decode_snapshot(*bytes);

  ASSERT_TRUE(restored.has_value());
  EXPECT_TRUE(restored->engine->sequence_exhausted());
  EXPECT_EQ(restored->engine->next_sequence(), Sequence{std::numeric_limits<std::uint64_t>::max()});
  EXPECT_EQ(restored->engine->last_logical_time(), 9U);
}

} // namespace
} // namespace matching_engine
