#include "domain/mesh/l4/circuit/CircuitServeDialPolicy.h"

#include <gtest/gtest.h>

#include <string>

TEST(CircuitServeDialPolicyTest, PeerIdOnlyRequiresLiveFarLeg) {
  EXPECT_FALSE(pbr::CircuitPeerIdOnlyHasLiveFarLeg(0));
  EXPECT_TRUE(pbr::CircuitPeerIdOnlyHasLiveFarLeg(1));
  EXPECT_EQ(pbr::kCircuitTargetPeerNotRegistered, "circuit target peer endpoint not registered");
}

TEST(CircuitServeDialPolicyTest, ClearMaWhenReserved) {
  EXPECT_TRUE(pbr::CircuitServeDialClearTargetMaWhenReserved(true, true));
  EXPECT_FALSE(pbr::CircuitServeDialClearTargetMaWhenReserved(true, false));
  EXPECT_FALSE(pbr::CircuitServeDialClearTargetMaWhenReserved(false, true));
}

TEST(CircuitServeDialPolicyTest, OpenOnLiveLinkWhenPeerIdOnly) {
  EXPECT_TRUE(pbr::CircuitServeDialOpenOnLiveLink(true));
  EXPECT_FALSE(pbr::CircuitServeDialOpenOnLiveLink(false));
}

TEST(CircuitServeDialPolicyTest, SkipPreferredAfterSeedPark) {
  EXPECT_TRUE(pbr::CallMediaShouldSkipPreferredDialAfterSeedPark(true, false));
  EXPECT_FALSE(pbr::CallMediaShouldSkipPreferredDialAfterSeedPark(true, true));
  EXPECT_FALSE(pbr::CallMediaShouldSkipPreferredDialAfterSeedPark(false, false));
  EXPECT_FALSE(pbr::CallMediaShouldSkipPreferredDialAfterSeedPark(false, true));
}

TEST(CircuitServeDialPolicyTest, ArmFarLegWaitDeadlineCapsToTunnel) {
  const int64_t now = 1000;
  const int64_t tunnel_deadline = now + 500;
  EXPECT_EQ(pbr::CircuitServeDialArmFarLegWaitDeadlineMs(now, tunnel_deadline),
            tunnel_deadline - pbr::kCircuitServeDialFarLegDeadlineSlackMs);
}

TEST(CircuitServeDialPolicyTest, ArmFarLegWaitDeadlineUsesMaxWhenNoTunnel) {
  const int64_t now = 1000;
  EXPECT_EQ(pbr::CircuitServeDialArmFarLegWaitDeadlineMs(now, 0),
            now + pbr::kCircuitServeDialFarLegWaitMs);
}
