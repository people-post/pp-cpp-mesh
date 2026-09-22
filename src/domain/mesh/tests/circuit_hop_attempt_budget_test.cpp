#include "domain/mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

TEST(CircuitHopAttemptBudgetTest, OrderStickyThenConnectedThenRest) {
  const std::vector<std::string> in = {"a", "b", "c", "d"};
  const std::unordered_set<std::string> connected = {"c", "d"};
  const auto out = pbr::OrderCircuitRelayAttempts(in, "b", [&](const std::string& key) {
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
  const auto out = pbr::OrderCircuitRelayAttempts(in, "", [](const std::string& key) {
    return key == "c";
  });
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "c");
  EXPECT_EQ(out[1], "a");
  EXPECT_EQ(out[2], "b");
}

TEST(CircuitHopAttemptBudgetTest, StartBridgeTimeoutClampsToEnvelope) {
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(10000, /*nested=*/false), pbr::kCircuitStartBridgeBaseMs);
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(10000, /*nested=*/true), pbr::kCircuitStartBridgeBaseMs);
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(5000, /*nested=*/true), 3000);
  // Last nested slice: spend remaining instead of returning 0 (was blocking 3rd try).
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(2500, /*nested=*/true), 2500);
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(1500, /*nested=*/false), 0);
  EXPECT_EQ(pbr::CircuitStartBridgeTimeoutMs(1500, /*nested=*/true), 0);
}

TEST(CircuitHopAttemptBudgetTest, NestedEstablishClamps) {
  EXPECT_EQ(pbr::CircuitNestedEstablishTimeoutMs(9000), pbr::kCircuitNestedEstablishBaseMs);
  EXPECT_EQ(pbr::CircuitNestedEstablishTimeoutMs(3000), 3000);
  EXPECT_EQ(pbr::CircuitNestedEstablishTimeoutMs(1000), 0);
}

TEST(CircuitHopAttemptBudgetTest, FastFailErrors) {
  EXPECT_TRUE(pbr::CircuitBridgeErrorIsFastFail("circuit hop reach failed: tunnel endpoint not registered"));
  EXPECT_TRUE(pbr::CircuitBridgeErrorIsFastFail("circuit target peer endpoint not dialable"));
  EXPECT_TRUE(pbr::CircuitBridgeErrorIsFastFail("relay preferred undialable"));
  EXPECT_TRUE(pbr::CircuitBridgeErrorIsFastFail("circuit-relay bridge timed out"));
  EXPECT_FALSE(pbr::CircuitBridgeErrorIsFastFail("circuit hop reach failed: tunnel timeout"));
}

TEST(CircuitHopAttemptBudgetTest, NotRegRetriesSameRelayWithoutSticky) {
  EXPECT_TRUE(pbr::CircuitShouldRetrySameRelayOnNotReg(true, 1));
  EXPECT_TRUE(pbr::CircuitShouldRetrySameRelayOnNotReg(true, 3));
  EXPECT_FALSE(pbr::CircuitShouldRetrySameRelayOnNotReg(true, pbr::kCircuitMaxStartBridgeAttempts));
  EXPECT_FALSE(pbr::CircuitShouldRetrySameRelayOnNotReg(false, 1));
}

TEST(CircuitHopAttemptBudgetTest, StickyRetryOnceOnFastFail) {
  EXPECT_TRUE(pbr::CircuitShouldRetryStickyOnce("seed", "seed", false, true, 1));
  EXPECT_FALSE(pbr::CircuitShouldRetryStickyOnce("seed", "seed", true, true, 2));
  EXPECT_FALSE(pbr::CircuitShouldRetryStickyOnce("other", "seed", false, true, 1));
  EXPECT_FALSE(pbr::CircuitShouldRetryStickyOnce("seed", "seed", false, false, 1));
  EXPECT_FALSE(pbr::CircuitShouldRetryStickyOnce("seed", "seed", false, true,
                                                 pbr::kCircuitMaxStartBridgeAttempts));
}
