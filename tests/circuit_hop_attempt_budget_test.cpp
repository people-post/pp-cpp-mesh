#include "mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

TEST(CircuitHopAttemptBudgetTest, OrderStickyThenConnectedThenRest) {
  const std::vector<std::string> in = {"a", "b", "c", "d"};
  const std::unordered_set<std::string> connected = {"c", "d"};
  const auto out = pp::mesh::OrderCircuitRelayAttempts(in, "b", [&](const std::string& key) {
    return connected.count(key) > 0;
  });
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], "b");
  EXPECT_EQ(out[1], "c");
  EXPECT_EQ(out[2], "d");
  EXPECT_EQ(out[3], "a");
}

TEST(CircuitHopAttemptBudgetTest, OrderWithoutStickyKeepsConnectedFirst) {
  const std::vector<std::string> in = {"a", "b", "c"};
  const auto out = pp::mesh::OrderCircuitRelayAttempts(in, "", [](const std::string& key) {
    return key == "c";
  });
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "c");
  EXPECT_EQ(out[1], "a");
  EXPECT_EQ(out[2], "b");
}

TEST(CircuitHopAttemptBudgetTest, StartBridgeTimeoutClampsToEnvelope) {
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(10000, /*nested=*/false), pp::mesh::kCircuitStartBridgeBaseMs);
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(10000, /*nested=*/true), pp::mesh::kCircuitStartBridgeBaseMs);
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(5000, /*nested=*/true), 3000);
  // Last nested slice: spend remaining instead of returning 0 (was blocking 3rd try).
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(2500, /*nested=*/true), 2500);
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(1500, /*nested=*/false), 0);
  EXPECT_EQ(pp::mesh::CircuitStartBridgeTimeoutMs(1500, /*nested=*/true), 0);
}

TEST(CircuitHopAttemptBudgetTest, NestedEstablishClamps) {
  EXPECT_EQ(pp::mesh::CircuitNestedEstablishTimeoutMs(9000), pp::mesh::kCircuitNestedEstablishBaseMs);
  EXPECT_EQ(pp::mesh::CircuitNestedEstablishTimeoutMs(3000), 3000);
  EXPECT_EQ(pp::mesh::CircuitNestedEstablishTimeoutMs(1000), 0);
}

TEST(CircuitHopAttemptBudgetTest, FastFailErrors) {
  EXPECT_TRUE(pp::mesh::CircuitBridgeErrorIsFastFail("circuit hop reach failed: tunnel endpoint not registered"));
  EXPECT_TRUE(pp::mesh::CircuitBridgeErrorIsFastFail("circuit target peer endpoint not dialable"));
  EXPECT_TRUE(pp::mesh::CircuitBridgeErrorIsFastFail("relay preferred undialable"));
  EXPECT_TRUE(pp::mesh::CircuitBridgeErrorIsFastFail("circuit-relay bridge timed out"));
  EXPECT_FALSE(pp::mesh::CircuitBridgeErrorIsFastFail("circuit hop reach failed: tunnel timeout"));
}

TEST(CircuitHopAttemptBudgetTest, NotRegRetriesSameRelayOnceThenAdvances) {
  EXPECT_TRUE(pp::mesh::CircuitShouldRetrySameRelayOnNotReg(true, 1));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetrySameRelayOnNotReg(true, 2));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetrySameRelayOnNotReg(true, 3));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetrySameRelayOnNotReg(true, pp::mesh::kCircuitMaxStartBridgeAttempts));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetrySameRelayOnNotReg(false, 1));
}

TEST(CircuitHopAttemptBudgetTest, StickyRetryOnceOnFastFail) {
  EXPECT_TRUE(pp::mesh::CircuitShouldRetryStickyOnce("seed", "seed", false, true, 1));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetryStickyOnce("seed", "seed", true, true, 2));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetryStickyOnce("other", "seed", false, true, 1));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetryStickyOnce("seed", "seed", false, false, 1));
  EXPECT_FALSE(pp::mesh::CircuitShouldRetryStickyOnce("seed", "seed", false, true,
                                                 pp::mesh::kCircuitMaxStartBridgeAttempts));
}
