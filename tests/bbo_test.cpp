#include "matching_engine/bbo.hpp"

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

} // namespace matching_engine