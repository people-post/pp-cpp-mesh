#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshPump.h"
#include "amp/link/PeerLinkManager.h"
#include "crypto/MlDsa.h"
#include "domain/mesh/reachability/Reachability.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <array>
#include <memory>
#include <string>

namespace pbr {
namespace {

TEST(AmpIpv6DialTest, PreferGlobalIpv6WhenRegisteringMixedAddrs) {
  // Mirrors CallMediaPlane RegisterCallPeerListenMultiaddrs: RegisterEndpoints(best-first).
  ASSERT_GE(sodium_init(), 0);
  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  std::array<uint8_t, 16> bytes{};
  bytes[0] = 0x20;
  bytes[1] = 0x01;
  bytes[2] = 0x0d;
  bytes[3] = 0xb8;
  bytes[15] = 0x55;
  const auto addr = pp::adp::IpEndpoint::V6(bytes, 19001);
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);
  auto ep = std::make_unique<pp::adp::Endpoint>(io, clock);
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  pp::amp::MshIdentity id;
  id.ml_dsa_secret_key = std::move(keys->secret_key);
  id.ml_dsa_public_key = std::move(keys->public_key);
  pp::amp::PeerLinkManager mgr(*ep, id, "QmLocal");

  const std::string lan = "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/QmRemote";
  auto v6 = pp::amp::FormatAdpMultiaddr(addr, "QmRemote");
  ASSERT_TRUE(static_cast<bool>(v6));
  const auto ranked = RankAmpDialMultiaddrs({lan, *v6});
  ASSERT_GE(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().find("/ip6/"), 0u);

  ASSERT_TRUE(static_cast<bool>(mgr.RegisterEndpoints("remote", ranked)));
  auto preferred = mgr.PreferredMultiaddr("remote");
  ASSERT_TRUE(preferred.has_value());
  EXPECT_EQ(preferred->find("/ip6/"), 0u) << *preferred;
  const auto* rec = mgr.Book().Find("remote");
  ASSERT_NE(rec, nullptr);
  ASSERT_EQ(rec->candidates.size(), 2u);
  EXPECT_EQ(rec->candidates.front().find("/ip6/"), 0u);
  EXPECT_TRUE(mgr.Book().AdvanceDialCandidate("remote"));
  EXPECT_EQ(mgr.Book().Find("remote")->multiaddr, lan);
}

TEST(AmpIpv6DialTest, EnsureAssociationOverMemoryIoIpv6) {
  ASSERT_GE(sodium_init(), 0);
  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  std::array<uint8_t, 16> a{};
  a[0] = 0x20;
  a[1] = 0x01;
  a[2] = 0x0d;
  a[3] = 0xb8;
  a[15] = 0x01;
  std::array<uint8_t, 16> b = a;
  b[15] = 0x02;
  const auto addr_a = pp::adp::IpEndpoint::V6(a, 1000);
  const auto addr_b = pp::adp::IpEndpoint::V6(b, 2000);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptEnabled(true);

  auto alice_keys = pp::MlDsa::GenerateKeyPair();
  auto bob_keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(alice_keys));
  ASSERT_TRUE(static_cast<bool>(bob_keys));
  pp::amp::MshIdentity alice;
  pp::amp::MshIdentity bob;
  alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
  alice.ml_dsa_public_key = std::move(alice_keys->public_key);
  bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
  bob.ml_dsa_public_key = std::move(bob_keys->public_key);

  pp::amp::PeerLinkManager mgr_a(*ep_a, alice, "QmAlice6");
  pp::amp::PeerLinkManager mgr_b(*ep_b, bob, "QmBob6");
  pp::amp::MeshPump pump_a(*ep_a, mgr_a);
  pp::amp::MeshPump pump_b(*ep_b, mgr_b);

  auto bob_ma = pp::amp::FormatAdpMultiaddr(addr_b, "QmBob6");
  ASSERT_TRUE(static_cast<bool>(bob_ma));
  ASSERT_TRUE(static_cast<bool>(mgr_a.RegisterEndpoint("bob", *bob_ma)));

  bool associated = false;
  std::string err;
  mgr_a.EnsureAssociation("bob", [&](pp::amp::PeerLinkManager::LinkRoe result) {
    associated = static_cast<bool>(result);
    if (!associated) {
      err = result.error().message;
    }
  });
  for (size_t i = 0; i < 500 && !(associated && mgr_b.FindConnectedInboundLink() != nullptr); ++i) {
    pump_a.Pump();
    pump_b.Pump();
    pump_a.Tick();
    pump_b.Tick();
  }
  EXPECT_TRUE(associated) << err;
  EXPECT_TRUE(mgr_a.IsConnected("bob"));
}

} // namespace
} // namespace pbr
