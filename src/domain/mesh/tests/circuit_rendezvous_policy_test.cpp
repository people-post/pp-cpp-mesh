#include "domain/mesh/l4/circuit/CircuitRendezvousPolicy.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

TEST(CircuitRendezvousPolicyTest, ParkOrderConnectedFirstStable) {
  const std::vector<std::string> in = {"a", "b", "c", "d"};
  const std::unordered_set<std::string> connected = {"c", "a"};
  const auto out = pbr::OrderRendezvousParkAttempts(in, [&](const std::string& key) {
    return connected.count(key) > 0;
  });
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "c");
  EXPECT_EQ(out[2], "b");
  EXPECT_EQ(out[3], "d");
}

TEST(CircuitRendezvousPolicyTest, ParkOrderWithoutConnectedKeepsInputOrder) {
  const std::vector<std::string> in = {"a", "b", "c"};
  const auto out = pbr::OrderRendezvousParkAttempts(in, [](const std::string&) { return false; });
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "b");
  EXPECT_EQ(out[2], "c");
}

TEST(CircuitRendezvousPolicyTest, CoverageKMatchesStartBridgeBudget) {
  EXPECT_EQ(pbr::kCircuitRendezvousParkCoverage, pbr::kCircuitMaxStartBridgeAttempts);
}

TEST(CircuitRendezvousPolicyTest, ColdDialCoverAllRemaining) {
  EXPECT_EQ(pbr::RendezvousColdDialLimit(5, 2, pbr::kCircuitRendezvousParkCoverage, true), 3u);
  EXPECT_EQ(pbr::RendezvousColdDialLimit(2, 2, pbr::kCircuitRendezvousParkCoverage, true), 0u);
  EXPECT_EQ(pbr::RendezvousColdDialLimit(0, 0, pbr::kCircuitRendezvousParkCoverage, true), 0u);
}

TEST(CircuitRendezvousPolicyTest, ColdDialMinimumCoverageOnly) {
  // K=4: 1 Connected → cold-dial 3 more.
  EXPECT_EQ(pbr::RendezvousColdDialLimit(6, 1, 4, false), 3u);
  // Already covered by Connected.
  EXPECT_EQ(pbr::RendezvousColdDialLimit(6, 4, 4, false), 0u);
  // Surface smaller than K → cold all remaining.
  EXPECT_EQ(pbr::RendezvousColdDialLimit(3, 1, 4, false), 2u);
}

TEST(CircuitRendezvousPolicyTest, ParkCoverageMet) {
  EXPECT_TRUE(pbr::RendezvousParkCoverageMet(4, 6, 4));
  EXPECT_TRUE(pbr::RendezvousParkCoverageMet(3, 3, 4)); // surface < K
  EXPECT_FALSE(pbr::RendezvousParkCoverageMet(2, 6, 4));
  EXPECT_FALSE(pbr::RendezvousParkCoverageMet(0, 0, 4));
}

TEST(CircuitRendezvousPolicyTest, DialerTopKCoveredByParkPrefix) {
  // Shared surface order (BuildCircuitHopList). Dialer may sticky-reorder, but answerer
  // covering the full surface (or ≥K) contains any dialer top-K after Connected band.
  const std::vector<std::string> surface = {"s1", "s2", "c1", "c2", "c3"};
  const auto park = pbr::OrderRendezvousParkAttempts(surface, [](const std::string& key) {
    return key == "s2";
  });
  ASSERT_GE(park.size(), pbr::kCircuitRendezvousParkCoverage);
  std::unordered_set<std::string> parked;
  for (std::size_t i = 0; i < pbr::kCircuitRendezvousParkCoverage; ++i) {
    parked.insert(park[i]);
  }
  // Connected s2 is first; prefix of K includes s2 plus next surface members.
  EXPECT_TRUE(parked.count("s2") > 0);
  EXPECT_EQ(park[0], "s2");
}
