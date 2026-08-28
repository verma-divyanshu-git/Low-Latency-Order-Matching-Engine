#include "matching_engine/bbo.hpp"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

namespace matching_engine {

TEST(BboSnapshotTest, PublishesAndReadsBestPricesAndQuantities) {
  BboSnapshot snapshot;
  const BboState state{.bid_price = Price{100},
                       .bid_quantity = Quantity{25U},
                       .ask_price = Price{101},
                       .ask_quantity = Quantity{30U}};

  snapshot.publish(state);

  EXPECT_EQ(snapshot.read(), std::optional<BboState>{state});
}

TEST(BboSnapshotTest, RepresentsEmptySides) {
  BboSnapshot snapshot;
  snapshot.publish(BboState{.bid_quantity = Quantity{0U},
                            .ask_price = Price{101},
                            .ask_quantity = Quantity{30U}});

  const auto state = snapshot.read();
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->bid_price.has_value());
  EXPECT_EQ(state->ask_price, std::optional<Price>{Price{101}});
}

TEST(BboSnapshotTest, ExhaustedReaderBudgetReportsNoConsistentRead) {
  BboSnapshot snapshot{0U};
  EXPECT_FALSE(snapshot.read().has_value());
}

TEST(BboSnapshotTest, ConcurrentReadersObserveOnlyCompletePublishedStates) {
  BboSnapshot snapshot{64U};
  const BboState first{.bid_price = Price{100},
                       .bid_quantity = Quantity{10U},
                       .ask_price = Price{101},
                       .ask_quantity = Quantity{20U}};
  const BboState second{.bid_price = Price{200},
                        .bid_quantity = Quantity{30U},
                        .ask_price = Price{201},
                        .ask_quantity = Quantity{40U}};
  snapshot.publish(first);

  std::atomic<bool> start{false};
  std::atomic<bool> writer_done{false};
  std::atomic<bool> observed_torn_state{false};
  std::thread writer{[&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::size_t iteration = 0U; iteration < 100'000U; ++iteration) {
      snapshot.publish((iteration & 1U) == 0U ? first : second);
    }
    writer_done.store(true, std::memory_order_release);
  }};
  std::thread reader{[&] {
    start.store(true, std::memory_order_release);
    while (!writer_done.load(std::memory_order_acquire)) {
      const auto state = snapshot.read();
      if (state.has_value() && *state != first && *state != second) {
        observed_torn_state.store(true, std::memory_order_release);
      }
    }
  }};

  writer.join();
  reader.join();
  EXPECT_FALSE(observed_torn_state.load(std::memory_order_acquire));
}

} // namespace matching_engine