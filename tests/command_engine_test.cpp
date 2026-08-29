#include "matching_engine/command.hpp"
#include "matching_engine/sequenced_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace matching_engine {
namespace {

static_assert(std::is_trivially_copyable_v<CommandPayload>);
static_assert(std::is_trivially_copyable_v<SequencedCommand>);
static_assert(std::is_trivially_copyable_v<EngineEvent>);
static_assert(sizeof(EngineEvent) == 64U);
static_assert(std::has_unique_object_representations_v<EngineEvent>);
static_assert(kEncodedCommandPayloadSize == 36U);

constexpr Handle kNoHandle{.index = kInvalidIndex, .generation = 0U};

TEST(CommandPayloadTest, CanonicalFactoriesCoverEveryCommandType) {
  const std::array payloads{
      CommandPayload::submit_limit(OrderId{7}, Side::sell, Price{-4}, Quantity{9},
                                   TimeInForce::fok),
      CommandPayload::submit_market(OrderId{8}, Side::buy, Quantity{10}),
      CommandPayload::cancel(Handle{2, 3}),
      CommandPayload::amend_quantity(Handle{4, 5}, Quantity{6}),
      CommandPayload::replace(Handle{7, 8}, Price{101}, Quantity{12}),
      CommandPayload::submit_iceberg(OrderId{9}, Side::sell, Price{100}, Quantity{20},
                 Quantity{4}),
      CommandPayload::submit_stop(OrderId{10}, Side::buy,
                  Price{std::numeric_limits<std::int64_t>::min()}, Quantity{3}),
      CommandPayload::submit_stop_limit(OrderId{11}, Side::sell,
                    Price{std::numeric_limits<std::int64_t>::max()},
                    Price{99}, Quantity{5}),
  };

  for (const CommandPayload& payload : payloads) {
    EXPECT_EQ(validate_command_payload(payload), CommandValidationError::none);
  }
}

TEST(CommandPayloadTest, StopTriggerUsesFullSignedInt64CarrierAndCanonicalMarketPrice) {
  const CommandPayload market = CommandPayload::submit_stop(
      OrderId{1U}, Side::buy, Price{std::numeric_limits<std::int64_t>::min()}, Quantity{2U});
  const CommandPayload limit = CommandPayload::submit_stop_limit(
      OrderId{2U}, Side::sell, Price{std::numeric_limits<std::int64_t>::max()}, Price{-7},
      Quantity{3U});

  EXPECT_EQ(market.price_ticks, 0);
  EXPECT_EQ(market.stop_trigger_price(), Price{std::numeric_limits<std::int64_t>::min()});
  EXPECT_EQ(limit.stop_trigger_price(), Price{std::numeric_limits<std::int64_t>::max()});
  EXPECT_EQ(limit.price_ticks, -7);

  CommandPayload malformed = market;
  malformed.price_ticks = 1;
  EXPECT_EQ(validate_command_payload(malformed), CommandValidationError::noncanonical);
}

TEST(CommandPayloadTest, RejectsInvalidEnumsAndNoncanonicalUnusedFields) {
  CommandPayload payload = CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{2});
  payload.side = static_cast<Side>(2U);
  EXPECT_EQ(validate_command_payload(payload), CommandValidationError::invalid_side);

  payload = CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{100}, Quantity{2},
                                         static_cast<TimeInForce>(3U));
  EXPECT_EQ(validate_command_payload(payload), CommandValidationError::invalid_time_in_force);

  payload = CommandPayload::cancel(Handle{1, 1});
  payload.price_ticks = 1;
  EXPECT_EQ(validate_command_payload(payload), CommandValidationError::noncanonical);

  payload.tag = static_cast<CommandType>(0xffU);
  EXPECT_EQ(validate_command_payload(payload), CommandValidationError::invalid_tag);
}

TEST(CommandEncodingTest, HasExactLittleEndianGoldenVector) {
  const CommandPayload payload =
      CommandPayload::submit_limit(OrderId{0x0102030405060708ULL}, Side::sell, Price{-2},
                                   Quantity{0x1112131415161718ULL}, TimeInForce::ioc);
  std::array<std::byte, kEncodedCommandPayloadSize> encoded{};
  ASSERT_EQ(encode_command_payload(payload, encoded), CommandCodecError::none);

  const std::array<std::byte, kEncodedCommandPayloadSize> golden{
      std::byte{0x01}, std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x08},
      std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
      std::byte{0x02}, std::byte{0x01}, std::byte{0xfe}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
      std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15}, std::byte{0x14},
      std::byte{0x13}, std::byte{0x12}, std::byte{0x11}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}};
  EXPECT_EQ(encoded, golden);

  const auto decoded = decode_command_payload(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, payload);
}

TEST(CommandEncodingTest, RejectsMalformedLengthReservedAndUnusedBytes) {
  std::array<std::byte, kEncodedCommandPayloadSize> encoded{};
  ASSERT_EQ(encode_command_payload(
                CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{2}), encoded),
            CommandCodecError::none);
  EXPECT_EQ(decode_command_payload(std::span<const std::byte>{encoded}.first(encoded.size() - 1U))
                .error(),
            CommandCodecError::invalid_length);

  encoded[3] = std::byte{1};
  EXPECT_EQ(decode_command_payload(encoded).error(), CommandCodecError::noncanonical);
  encoded[3] = std::byte{0};
  encoded[12] = std::byte{1};
  EXPECT_EQ(decode_command_payload(encoded).error(), CommandCodecError::noncanonical);
}

TEST(SequencerTest, StampsMonotonicLogicalTimeWithoutReadingAClock) {
  Sequencer sequencer;
  const CommandPayload payload = CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{2});

  const auto first = sequencer.stamp(payload, 10U);
  const auto second = sequencer.stamp(payload, 10U);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->sequence, Sequence{1});
  EXPECT_EQ(second->sequence, Sequence{2});
  EXPECT_EQ(second->logical_time, 10U);

  EXPECT_EQ(sequencer.stamp(payload, 9U).error(), SequencerError::decreasing_logical_time);
  EXPECT_EQ(sequencer.next_sequence(), Sequence{3});
}

TEST(SequencerTest, RejectsExhaustionWithoutAdvancing) {
  Sequencer sequencer{Sequence{std::numeric_limits<std::uint64_t>::max()}, 7U};
  const CommandPayload payload = CommandPayload::cancel(Handle{1, 1});
  const auto final = sequencer.stamp(payload, 7U);
  ASSERT_TRUE(final.has_value());
  EXPECT_EQ(final->sequence, Sequence{std::numeric_limits<std::uint64_t>::max()});

  const auto result = sequencer.stamp(payload, 7U);
  EXPECT_EQ(result.error(), SequencerError::sequence_exhausted);
  EXPECT_EQ(sequencer.next_sequence(), Sequence{std::numeric_limits<std::uint64_t>::max()});
}

class SequencedEngineTest : public ::testing::Test {
public:
  SequencedEngine engine{PriceDomain{Price{100}, 11U}, 8U, Quantity{100U}};
  Sequencer sequencer;
  std::array<EngineEvent, 17> events{};

  ApplyResult apply(const CommandPayload& payload) {
    const auto command = sequencer.stamp(payload, sequencer.last_logical_time() + 1U);
    EXPECT_TRUE(command.has_value());
    return engine.apply(*command, events);
  }
};

TEST_F(SequencedEngineTest, EmitsResultThenTradesInExactOrder) {
  const ApplyResult first =
      apply(CommandPayload::submit_limit(OrderId{11}, Side::sell, Price{104}, Quantity{2}));
  ASSERT_EQ(first, (ApplyResult{ApplyStatus::applied, 1U}));
  const Handle first_handle = events[0].handle;
  ASSERT_NE(first_handle, kNoHandle);
  ASSERT_EQ(
      apply(CommandPayload::submit_limit(OrderId{12}, Side::sell, Price{104}, Quantity{3})).status,
      ApplyStatus::applied);

  const ApplyResult aggressive =
      apply(CommandPayload::submit_market(OrderId{20}, Side::buy, Quantity{4}));
  ASSERT_EQ(aggressive, (ApplyResult{ApplyStatus::applied, 3U}));
  EXPECT_EQ(events[0].type, EngineEventType::submit_result);
  EXPECT_EQ(events[0].command_sequence, Sequence{3});
  EXPECT_EQ(events[0].event_index, 0U);
  EXPECT_EQ(events[0].quantity, Quantity{4});
  EXPECT_EQ(events[0].secondary_quantity, Quantity{0});
  EXPECT_EQ(events[1],
            (EngineEvent::trade(Sequence{3}, 1U,
                                Trade{OrderId{20}, OrderId{11}, Price{104}, Quantity{2}})));
  EXPECT_EQ(events[2],
            (EngineEvent::trade(Sequence{3}, 2U,
                                Trade{OrderId{20}, OrderId{12}, Price{104}, Quantity{2}})));
  EXPECT_EQ(engine.order_book().order_info(first_handle), std::nullopt);
}

TEST_F(SequencedEngineTest, EmitsCancelAmendReplaceAndRejectionState) {
  ASSERT_EQ(
      apply(CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{102}, Quantity{9})).status,
      ApplyStatus::applied);
  const Handle handle = events[0].handle;

  ASSERT_EQ(apply(CommandPayload::amend_quantity(handle, Quantity{6})).event_count, 1U);
  EXPECT_EQ(events[0].type, EngineEventType::amend_result);
  EXPECT_EQ(events[0].quantity, Quantity{9});
  EXPECT_EQ(events[0].secondary_quantity, Quantity{6});
  EXPECT_EQ(events[0].handle, handle);

  ASSERT_EQ(apply(CommandPayload::replace(handle, Price{103}, Quantity{5})).event_count, 1U);
  EXPECT_EQ(events[0].type, EngineEventType::submit_result);
  EXPECT_EQ(events[0].quantity, Quantity{0});
  EXPECT_EQ(events[0].secondary_quantity, Quantity{5});
  const Handle replacement = events[0].handle;
  EXPECT_NE(replacement, handle);

  ASSERT_EQ(apply(CommandPayload::cancel(replacement)).event_count, 1U);
  EXPECT_EQ(events[0].type, EngineEventType::cancel_result);
  EXPECT_EQ(events[0].order_id, OrderId{1});
  EXPECT_EQ(events[0].quantity, Quantity{5});

  ASSERT_EQ(apply(CommandPayload::cancel(replacement)).event_count, 1U);
  EXPECT_EQ(events[0].reason, static_cast<std::uint8_t>(CancelReason::invalid_handle));
}

TEST_F(SequencedEngineTest, InsufficientEventSpanDoesNotMutateBook) {
  const auto command = sequencer.stamp(
      CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{102}, Quantity{9}), 1U);
  ASSERT_TRUE(command.has_value());

  const ApplyResult result = engine.apply(*command, std::span<EngineEvent>{});

  EXPECT_EQ(result, (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_EQ(engine.order_book().best_bid(), std::nullopt);
  EXPECT_EQ(engine.order_book().check_invariants().violation, InvariantViolation::none);
}

TEST_F(SequencedEngineTest, RejectsNoncanonicalCommandBeforeMutation) {
  auto command = sequencer.stamp(CommandPayload::cancel(Handle{1, 1}), 1U);
  ASSERT_TRUE(command.has_value());
  command->payload.order_id = 1U;

  EXPECT_EQ(engine.apply(*command, events), (ApplyResult{ApplyStatus::invalid_command, 0U}));
  EXPECT_EQ(engine.order_book().check_invariants().reachable_count, 0U);
}

TEST(SequencedEngineCapacityTest, MarketRequiresResultPlusEveryPossibleMaker) {
  SequencedEngine engine{PriceDomain{Price{100}, 11U}, 2U, Quantity{100U}};
  std::array<EngineEvent, 3> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1}, Side::sell, Price{104}, Quantity{1}),
                  Sequence{1}, 1U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{2}, Side::sell, Price{105}, Quantity{1}),
                  Sequence{2}, 2U},
                 events)
          .status,
      ApplyStatus::applied);
  const SequencedCommand market{CommandPayload::submit_market(OrderId{3}, Side::buy, Quantity{2}),
                                Sequence{3}, 3U};

  EXPECT_EQ(engine.apply(market, std::span<EngineEvent>{events}.first(2U)),
            (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_EQ(engine.order_book().best_ask(), Price{104});

  EXPECT_EQ(engine.apply(market, events), (ApplyResult{ApplyStatus::applied, 3U}));
  EXPECT_EQ(engine.order_book().best_ask(), std::nullopt);
}

TEST(SequencedEngineCapacityTest, StopCascadePreflightsDormantStopBound) {
  SequencedEngine engine{PriceDomain{Price{0}, 201U}, 2U, Quantity{10U}};
  std::array<EngineEvent, 5U> events{};
  ASSERT_EQ(engine.apply({CommandPayload::submit_stop(OrderId{1U}, Side::buy, Price{100},
                                                        Quantity{1U}),
                          Sequence{1U}, 1U},
                         events)
                .status,
            ApplyStatus::applied);
  const Handle stop = events[0].handle;

  ASSERT_EQ(engine.apply({CommandPayload::submit_limit(OrderId{2U}, Side::sell, Price{100},
                                                       Quantity{1U}),
                          Sequence{2U}, 2U},
                         events)
                .status,
            ApplyStatus::applied);
  const SequencedCommand trigger{CommandPayload::submit_market(OrderId{3U}, Side::buy,
                                                                Quantity{1U}),
                                 Sequence{3U}, 3U};
  EXPECT_EQ(engine.apply(trigger, std::span<EngineEvent>{events}.first(3U)),
            (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_TRUE(engine.order_book().order_info(stop).has_value());
  EXPECT_EQ(engine.order_book().best_ask(), Price{100});

  EXPECT_EQ(engine.apply(trigger, std::span<EngineEvent>{events}.first(4U)),
            (ApplyResult{ApplyStatus::applied, 3U}));
  EXPECT_EQ(events[1].type, EngineEventType::trade);
  EXPECT_EQ(events[2], (EngineEvent{.command_sequence = Sequence{3U},
                                    .order_id = OrderId{1U},
                                    .quantity = Quantity{0U},
                                    .secondary_quantity = Quantity{1U},
                                    .handle = Handle{kInvalidIndex, 0U},
                                    .event_index = 2U,
                                    .type = EngineEventType::stop_triggered}));
  EXPECT_FALSE(engine.order_book().order_info(stop).has_value());
}

TEST(SequencedEngineCapacityTest, LiveReplaceNeedsAtMostArenaCapacityTotalEvents) {
  SequencedEngine engine{PriceDomain{Price{100}, 11U}, 3U, Quantity{100U}};
  std::array<EngineEvent, 7> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{100}, Quantity{3}),
                  Sequence{1}, 1U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle replaced = events[0].handle;
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{2}, Side::sell, Price{103}, Quantity{1}),
                  Sequence{2}, 2U},
                 events)
          .status,
      ApplyStatus::applied);
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{3}, Side::sell, Price{104}, Quantity{1}),
                  Sequence{3}, 3U},
                 events)
          .status,
      ApplyStatus::applied);
  const SequencedCommand replace{CommandPayload::replace(replaced, Price{105}, Quantity{2}),
                                 Sequence{4}, 4U};

  EXPECT_EQ(engine.apply(replace, std::span<EngineEvent>{events}.first(2U)),
            (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_EQ(engine.order_book().order_info(replaced)->remaining, Quantity{3});

  EXPECT_EQ(engine.apply(replace, std::span<EngineEvent>{events}.first(3U)),
            (ApplyResult{ApplyStatus::applied, 3U}));
  EXPECT_EQ(events[1].type, EngineEventType::trade);
  EXPECT_EQ(events[2].type, EngineEventType::trade);
}

TEST(SequencedEngineCapacityTest, InvalidReplaceAndZeroArenaNeedOneEvent) {
  std::array<EngineEvent, 1> event{};
  SequencedEngine empty{PriceDomain{Price{100}, 1U}, 0U, Quantity{100U}};
  const SequencedCommand replace{CommandPayload::replace(Handle{0, 1}, Price{100}, Quantity{1}),
                                 Sequence{1}, 1U};
  EXPECT_EQ(empty.apply(replace, std::span<EngineEvent>{}),
            (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_EQ(empty.apply(replace, event), (ApplyResult{ApplyStatus::applied, 1U}));
  EXPECT_EQ(event[0].reason, static_cast<std::uint8_t>(RejectReason::invalid_handle));

  const SequencedCommand market{CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{1}),
                                Sequence{2}, 2U};
  EXPECT_EQ(empty.apply(market, std::span<EngineEvent>{}),
            (ApplyResult{ApplyStatus::insufficient_event_capacity, 0U}));
  EXPECT_EQ(empty.apply(market, event), (ApplyResult{ApplyStatus::applied, 1U}));
}

TEST(SequencedEngineCapacityTest, CertainReplaceRejectionsNeedOneEvent) {
  SequencedEngine engine{PriceDomain{Price{100}, 3U}, 2U, Quantity{10U}};
  std::array<EngineEvent, 3> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{100}, Quantity{5}),
                  Sequence{1}, 1U},
                 events)
          .status,
      ApplyStatus::applied);
  const Handle handle = events[0].handle;
  const std::array payloads{
      CommandPayload::replace(Handle{kInvalidIndex, 1U}, Price{101}, Quantity{1}),
      CommandPayload::replace(handle, Price{101}, Quantity{0}),
      CommandPayload::replace(handle, Price{101}, Quantity{11}),
      CommandPayload::replace(handle, Price{99}, Quantity{1}),
  };
  const std::array reasons{RejectReason::invalid_handle, RejectReason::zero_quantity,
                           RejectReason::quantity_too_large, RejectReason::price_out_of_domain};

  for (std::size_t index = 0U; index < payloads.size(); ++index) {
    const SequencedCommand command{payloads[index], Sequence{index + 2U}, index + 2U};
    EXPECT_EQ(engine.apply(command, std::span<EngineEvent>{events}.first(1U)),
              (ApplyResult{ApplyStatus::applied, 1U}));
    EXPECT_EQ(events[0].reason, static_cast<std::uint8_t>(reasons[index]));
    EXPECT_EQ(engine.order_book().order_info(handle)->remaining, Quantity{5});
  }
}

TEST(SequencedEngineCapacityTest, CertainSubmitRejectionsNeedOneEvent) {
  SequencedEngine engine{PriceDomain{Price{100}, 3U}, 1U, Quantity{10U}};
  std::array<EngineEvent, 2> events{};
  const std::array payloads{
      CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{100}, Quantity{0}),
      CommandPayload::submit_limit(OrderId{2}, Side::buy, Price{100}, Quantity{11}),
      CommandPayload::submit_limit(OrderId{3}, Side::buy, Price{99}, Quantity{1}),
      CommandPayload::submit_limit(OrderId{4}, Side::buy, Price{102}, Quantity{1},
                                   TimeInForce::fok),
      CommandPayload::submit_market(OrderId{5}, Side::buy, Quantity{0}),
      CommandPayload::submit_market(OrderId{6}, Side::buy, Quantity{11}),
  };
  const std::array reasons{
      RejectReason::zero_quantity,       RejectReason::quantity_too_large,
      RejectReason::price_out_of_domain, RejectReason::fok_not_fillable,
      RejectReason::zero_quantity,       RejectReason::quantity_too_large,
  };

  for (std::size_t index = 0U; index < payloads.size(); ++index) {
    const SequencedCommand command{payloads[index], Sequence{index + 1U}, index + 1U};
    EXPECT_EQ(engine.apply(command, std::span<EngineEvent>{events}.first(1U)),
              (ApplyResult{ApplyStatus::applied, 1U}));
    EXPECT_EQ(events[0].reason, static_cast<std::uint8_t>(reasons[index]));
  }
}

TEST(SequencedEngineCapacityTest, CertainFullArenaRejectionNeedsOneEvent) {
  SequencedEngine engine{PriceDomain{Price{100}, 3U}, 1U, Quantity{10U}};
  std::array<EngineEvent, 2> events{};
  ASSERT_EQ(
      engine
          .apply({CommandPayload::submit_limit(OrderId{1}, Side::buy, Price{100}, Quantity{1}),
                  Sequence{1}, 1U},
                 events)
          .status,
      ApplyStatus::applied);

  const SequencedCommand rejected{
      CommandPayload::submit_limit(OrderId{2}, Side::buy, Price{101}, Quantity{1}), Sequence{2},
      2U};
  EXPECT_EQ(engine.apply(rejected, std::span<EngineEvent>{events}.first(1U)),
            (ApplyResult{ApplyStatus::applied, 1U}));
  EXPECT_EQ(events[0].reason, static_cast<std::uint8_t>(RejectReason::order_capacity_exhausted));
}

TEST(SequencedEngineSequenceTest, AcceptsMaximumOnceThenRemainsExhausted) {
  SequencedEngine engine{PriceDomain{Price{100}, 1U}, 0U, Quantity{100U},
                         Sequence{std::numeric_limits<std::uint64_t>::max()}, 7U};
  std::array<EngineEvent, 1> event{};
  const SequencedCommand final{CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{1}),
                               Sequence{std::numeric_limits<std::uint64_t>::max()}, 7U};
  EXPECT_EQ(engine.apply(final, event), (ApplyResult{ApplyStatus::applied, 1U}));

  const InvariantResult before = engine.order_book().check_invariants();
  EXPECT_EQ(engine.apply(final, event), (ApplyResult{ApplyStatus::sequence_exhausted, 0U}));
  EXPECT_EQ(engine.order_book().check_invariants(), before);
}

TEST(SequencedEngineSequenceTest, RejectsSequenceZeroAtInitialState) {
  SequencedEngine engine{PriceDomain{Price{100}, 1U}, 0U, Quantity{100U}};
  std::array<EngineEvent, 1> event{};
  EXPECT_EQ(engine.apply({CommandPayload::submit_market(OrderId{1}, Side::buy, Quantity{1}),
                          Sequence{0}, 0U},
                         event),
            (ApplyResult{ApplyStatus::invalid_sequence, 0U}));
}

TEST(SequencedEngineSequenceTest, RejectsConfiguredInitialSequenceZero) {
  EXPECT_THROW((SequencedEngine{PriceDomain{Price{100}, 1U}, 0U, Quantity{100U}, Sequence{0}, 0U}),
               std::invalid_argument);
}

} // namespace
} // namespace matching_engine
