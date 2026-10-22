#include "mesh/l4/circuit/CircuitServeDialPolicy.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"

#include <gtest/gtest.h>

#include <string>

TEST(CircuitServeDialPolicyTest, PeerIdOnlyRequiresLiveFarLeg) {
  EXPECT_FALSE(pp::mesh::CircuitPeerIdOnlyHasLiveFarLeg(0));
  EXPECT_TRUE(pp::mesh::CircuitPeerIdOnlyHasLiveFarLeg(1));
  EXPECT_EQ(pp::mesh::kCircuitTargetPeerNotRegistered, "circuit target peer endpoint not registered");
  // Relays before the ack's `code` sent that text only: it still reads as not reserved.
  EXPECT_EQ(pp::mesh::CircuitRefusalFromAck("", pp::mesh::kCircuitTargetPeerNotRegistered),
            pp::mesh::CircuitRefusal::TargetNotReserved);
}

TEST(CircuitServeDialPolicyTest, ClearMaWhenReserved) {
  EXPECT_TRUE(pp::mesh::CircuitServeDialClearTargetMaWhenReserved(true, true));
  EXPECT_FALSE(pp::mesh::CircuitServeDialClearTargetMaWhenReserved(true, false));
  EXPECT_FALSE(pp::mesh::CircuitServeDialClearTargetMaWhenReserved(false, true));
}

TEST(CircuitServeDialPolicyTest, OpenOnLiveLinkWhenPeerIdOnly) {
  EXPECT_TRUE(pp::mesh::CircuitServeDialOpenOnLiveLink(true));
  EXPECT_FALSE(pp::mesh::CircuitServeDialOpenOnLiveLink(false));
}

TEST(CircuitServeDialPolicyTest, SkipPreferredAfterSeedPark) {
  EXPECT_TRUE(pp::mesh::ShouldSkipPrivatePreferredDialAfterSeedPark(true, false));
  EXPECT_FALSE(pp::mesh::ShouldSkipPrivatePreferredDialAfterSeedPark(true, true));
  EXPECT_FALSE(pp::mesh::ShouldSkipPrivatePreferredDialAfterSeedPark(false, false));
  EXPECT_FALSE(pp::mesh::ShouldSkipPrivatePreferredDialAfterSeedPark(false, true));
}

TEST(CircuitServeDialPolicyTest, ArmFarLegWaitDeadlineCapsToTunnel) {
  const int64_t now = 1000;
  const int64_t tunnel_deadline = now + 4000;
  EXPECT_EQ(pp::mesh::CircuitServeDialArmFarLegWaitDeadlineMs(now, tunnel_deadline),
            tunnel_deadline - pp::mesh::kCircuitServeDialFarLegDeadlineSlackMs);
}

TEST(CircuitServeDialPolicyTest, ArmFarLegWaitDeadlineUsesTunnelWhenShorterThanSlack) {
  const int64_t now = 1000;
  const int64_t tunnel_deadline = now + 500; // shorter than slack
  EXPECT_EQ(pp::mesh::CircuitServeDialArmFarLegWaitDeadlineMs(now, tunnel_deadline), tunnel_deadline);
}

TEST(CircuitServeDialPolicyTest, ArmFarLegWaitDeadlineUsesMaxWhenNoTunnel) {
  const int64_t now = 1000;
  EXPECT_EQ(pp::mesh::CircuitServeDialArmFarLegWaitDeadlineMs(now, 0),
            now + pp::mesh::kCircuitServeDialFarLegWaitMs);
}

// A relay's refusal by its ack's code (docs/ERRORS.md); the text only for relays before codes.
TEST(CircuitRefusalTest, TheAckCodeDecidesAndOldTextStillReads) {
  using pp::mesh::CircuitRefusal;
  using pp::mesh::CircuitRefusalFromAck;
  using pp::mesh::CircuitRefusalWire;
  for (const CircuitRefusal refusal : {CircuitRefusal::TargetNotReserved, CircuitRefusal::TargetUnreachable,
                                       CircuitRefusal::Busy, CircuitRefusal::Refused, CircuitRefusal::NotReady,
                                       CircuitRefusal::BadRequest}) {
    EXPECT_EQ(CircuitRefusalFromAck(CircuitRefusalWire(refusal), "unrelated words"), refusal);
  }
  EXPECT_EQ(CircuitRefusalFromAck("a_newer_code", "relay busy"), CircuitRefusal::Generic);
  EXPECT_EQ(CircuitRefusalFromAck("", "relay busy: standby refused"), CircuitRefusal::Busy);
  EXPECT_EQ(CircuitRefusalFromAck("", "relay scope: stranger refused"), CircuitRefusal::Refused);
  EXPECT_EQ(CircuitRefusalFromAck("", "relay target stream timed out"), CircuitRefusal::TargetUnreachable);
  EXPECT_EQ(CircuitRefusalFromAck("", "unsupported op"), CircuitRefusal::BadRequest);
}
