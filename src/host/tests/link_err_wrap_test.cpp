// Each owning layer that wraps Amp's link failures reads a link VersionMismatch — the peer speaks another wire or
// MSH version — as its own VersionMismatch ("update needed", docs/ERRORS.md), never as a plain link failure.
#include "amp/link/PeerLinkManager.h"
#include "mesh/dht/client/DhtClient.h"
#include "mesh/discovery/client/DirectoryClient.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "mesh/reachability/dial_back/DialBackTypes.h"
#include "mesh/reachability/punch/PunchLinkOps.h"

#include <gtest/gtest.h>

#include <string>

namespace pp::mesh {
namespace {

using AmpErr = pp::amp::PeerLinkManager::Err;

pp::amp::PeerLinkManager::Failure Link(const AmpErr code) {
  return pp::amp::PeerLinkManager::Failure::Of(code, "amp link: the peer speaks wire v3, not v2");
}

template <typename Failure>
void ExpectKeepsAmpsText(const Failure& failure) {
  EXPECT_NE(failure.message.find("wire v3"), std::string::npos) << failure.message;
}

TEST(LinkErrWrapTest, ALinkVersionMismatchIsEachLayersVersionMismatch) {
  const auto link = Link(AmpErr::VersionMismatch);

  const auto dht = DhtClient::WrapLinkFailure(link);
  EXPECT_EQ(dht.GetCode(), DhtClient::Err::VersionMismatch);
  EXPECT_TRUE(DhtClient::IsRefusal(dht.GetCode())) << "a publish every peer refuses so backs off (T11)";
  ExpectKeepsAmpsText(dht);

  const auto directory = DirectoryClient::WrapLinkFailure(link);
  EXPECT_EQ(directory.GetCode(), DirectoryClient::Err::VersionMismatch);
  ExpectKeepsAmpsText(directory);

  const auto dial_back = WrapDialBackLinkFailure(link);
  EXPECT_EQ(dial_back.GetCode(), DialBackErr::VersionMismatch);
  ExpectKeepsAmpsText(dial_back);

  const auto punch = WrapPunchLinkFailure(link);
  EXPECT_EQ(punch.GetCode(), PunchErr::VersionMismatch) << "the walk asks another introducer";
  ExpectKeepsAmpsText(punch);

  const auto circuit = CircuitClientCoordinator::WrapLinkFailure(link);
  EXPECT_EQ(circuit.GetCode(), CircuitTunnelErr::VersionMismatch) << "circuit reach holds the relay back";
  ExpectKeepsAmpsText(circuit);

  const auto media_relay = MediaRelayClientCoordinator::WrapLinkFailure(link);
  EXPECT_EQ(media_relay.GetCode(), MediaRelayErr::VersionMismatch);
  ExpectKeepsAmpsText(media_relay);
}

// Amp's Generic stays each layer's Generic (or its plain link failure), as before.
TEST(LinkErrWrapTest, ALinkGenericStaysUnclassified) {
  const auto link = Link(AmpErr::Generic);
  EXPECT_EQ(DhtClient::WrapLinkFailure(link).GetCode(), DhtClient::Err::Generic);
  EXPECT_EQ(DirectoryClient::WrapLinkFailure(link).GetCode(), DirectoryClient::Err::Generic);
  EXPECT_EQ(WrapDialBackLinkFailure(link).GetCode(), DialBackErr::Generic);
  EXPECT_EQ(WrapPunchLinkFailure(link).GetCode(), PunchErr::Generic);
  EXPECT_EQ(CircuitClientCoordinator::WrapLinkFailure(link).GetCode(), CircuitTunnelErr::RelayLinkFailed);
  EXPECT_EQ(MediaRelayClientCoordinator::WrapLinkFailure(link).GetCode(), MediaRelayErr::LinkFailed);
}

} // namespace
} // namespace pp::mesh
