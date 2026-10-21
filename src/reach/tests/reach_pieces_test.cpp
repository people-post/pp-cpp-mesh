#include "mesh/reach/AmpCircuitHopReach.h"
#include "mesh/reach/CircuitRendezvousCoordinator.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "mesh/reach/PunchIntroducerWalk.h"

#include <gtest/gtest.h>
#include <optional>
#include <string>
namespace pp::mesh {
namespace {

// No mesh (before start / after stop): the punch step answers at once instead of hanging reach.
TEST(PunchIntroducerWalkTest, WithoutAMeshPunchesFailAtOnce) {
  PunchIntroducerWalk walk;
  std::optional<PunchIntroducerWalk::WalkRoe> cold;
  walk.TryColdPunchAsync("12D3KooWTarget", [&](PunchIntroducerWalk::WalkRoe r) { cold = std::move(r); });
  ASSERT_TRUE(cold.has_value());
  EXPECT_FALSE(*cold);
  std::optional<PunchIntroducerWalk::WalkRoe> upgrade;
  walk.TryUpgradePunchAsync("12D3KooWIntro", "12D3KooWTarget", [&](PunchIntroducerWalk::WalkRoe r) { upgrade = std::move(r); });
  ASSERT_TRUE(upgrade.has_value());
  EXPECT_FALSE(*upgrade);
}

// No mesh: no dial surface, and a park request answers `false` (H010 callers must not wait forever).
TEST(CircuitRendezvousCoordinatorTest, WithoutAMeshNothingIsDialableAndParkingAnswersFalse) {
  CircuitRendezvousCoordinator rendezvous;
  CircuitRendezvousDeps deps;
  deps.rendezvous_candidates = []() {
    MeshHopCandidate hop;
    hop.peer_id = "12D3KooWSeed";
    return std::vector<MeshHopCandidate>{hop};
  };
  rendezvous.SetDeps(std::move(deps));
  EXPECT_TRUE(rendezvous.DialableRelayIds("").empty());
  std::optional<bool> parked;
  rendezvous.EnsureBootstrapSeedParkedAsync([&](bool ok) { parked = ok; }, 50);
  ASSERT_TRUE(parked.has_value());
  EXPECT_FALSE(*parked);
}

// The walk moves to the next introducer only when this one cannot get to the target.
TEST(PunchIntroducerWalkTest, AnotherIntroducerOnlyWhenThisOneCannotGetThere) {
  EXPECT_TRUE(PunchIntroducerWalk::AnotherIntroducerMayHelp(PunchErr::TargetUnknown));
  EXPECT_TRUE(PunchIntroducerWalk::AnotherIntroducerMayHelp(PunchErr::EndpointNotRegistered));
  EXPECT_TRUE(PunchIntroducerWalk::AnotherIntroducerMayHelp(PunchErr::LinkFailed));
  EXPECT_FALSE(PunchIntroducerWalk::AnotherIntroducerMayHelp(PunchErr::Declined)) << "the target said no";
  EXPECT_FALSE(PunchIntroducerWalk::AnotherIntroducerMayHelp(PunchErr::PunchFailed)) << "it ran; no path";
  const auto declined = PunchIntroducerWalk::WrapPunchFailure(PunchFailure::Of(PunchErr::Declined, "no"));
  EXPECT_EQ(declined.GetCode(), PunchIntroducerWalk::Err::Declined);
  EXPECT_NE(declined.message.find("no"), std::string::npos) << "the punch's chain stays in the message";
}

// A bridge's failure, as circuit reach reports it: the relay's answer kept, its link trouble pooled.
TEST(AmpCircuitHopReachTest, TunnelFailuresMapToReachCodes) {
  const auto wrap = [](const CircuitTunnelErr code) {
    return AmpCircuitHopReach::WrapTunnelFailure(CircuitClientCoordinator::Failure::Of(code, "x")).GetCode();
  };
  EXPECT_EQ(wrap(CircuitTunnelErr::TargetNotReserved), CircuitReachErr::TargetNotReserved);
  EXPECT_EQ(wrap(CircuitTunnelErr::TargetUnreachable), CircuitReachErr::TargetUnreachable);
  EXPECT_EQ(wrap(CircuitTunnelErr::RelayBusy), CircuitReachErr::RelayRefused);
  EXPECT_EQ(wrap(CircuitTunnelErr::RelayLinkFailed), CircuitReachErr::RelayUnreachable);
  EXPECT_EQ(wrap(CircuitTunnelErr::Closed), CircuitReachErr::RelayUnreachable);
  EXPECT_EQ(wrap(CircuitTunnelErr::RelayNoEndpoint), CircuitReachErr::RelayUnusable);
  EXPECT_EQ(wrap(CircuitTunnelErr::Timeout), CircuitReachErr::Timeout);
  EXPECT_EQ(wrap(CircuitTunnelErr::Aborted), CircuitReachErr::Aborted);
}

// Reach keeps a dial's meaning (a stale link wants a fresh dial) and pools the circuit's.
TEST(PeerReachCoordinatorCodesTest, LinkAndCircuitFailuresMapToReachCodes) {
  using LinkErr = IChatPeerLinks::Err;
  const auto link = [](const LinkErr code) {
    return PeerReachCoordinator::WrapLinkFailure(IChatPeerLinks::Failure::Of(code, "x")).GetCode();
  };
  EXPECT_EQ(link(LinkErr::TransportFailed), PeerReachCoordinator::Err::TransportFailed);
  EXPECT_EQ(link(LinkErr::DialTimeout), PeerReachCoordinator::Err::NoAnswer);
  EXPECT_EQ(link(LinkErr::HandshakeFailed), PeerReachCoordinator::Err::Refused);
  EXPECT_EQ(link(LinkErr::MaxLinksReached), PeerReachCoordinator::Err::DialBusy);
  EXPECT_EQ(link(LinkErr::EndpointNotRegistered), PeerReachCoordinator::Err::NoEndpoint);
  const auto circuit = [](const CircuitReachErr code) {
    return PeerReachCoordinator::WrapCircuitFailure(CircuitReachFailure::Of(code, "x")).GetCode();
  };
  EXPECT_EQ(circuit(CircuitReachErr::TargetNotReserved), PeerReachCoordinator::Err::CircuitFailed);
  EXPECT_EQ(circuit(CircuitReachErr::Aborted), PeerReachCoordinator::Err::Cancelled);
}

} // namespace
} // namespace pp::mesh
