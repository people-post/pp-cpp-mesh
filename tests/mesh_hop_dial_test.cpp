#include "mesh/types/MeshHopDial.h"
#include "mesh/types/RelayScope.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace pp::mesh {
namespace {

TEST(MeshHopDialTest, RelayAdmissionAllowsContactOrPublicStrangers) {
  const std::unordered_set<std::string> contacts = {"friend"};
  EXPECT_TRUE(RelayAdmissionAllowsDialer(kRelayScopeVolunteerServe, "friend", contacts));
  EXPECT_FALSE(RelayAdmissionAllowsDialer(kRelayScopeVolunteerServe, "stranger", contacts));
  EXPECT_TRUE(RelayAdmissionAllowsDialer(kRelayScopeVolunteerServe, "stranger", {}));
  const RelayScopeMask with_public =
      kRelayScopeVolunteerServe | static_cast<RelayScopeMask>(RelayScope::Public);
  EXPECT_TRUE(RelayAdmissionAllowsDialer(with_public, "stranger", contacts));
}

TEST(MeshHopDialTest, MultiaddrHasPrivateIpv4Host) {
  EXPECT_TRUE(MultiaddrHasPrivateIpv4Host("/ip4/192.168.1.132/udp/59286/adp/1.0.0/p2p/x"));
  EXPECT_TRUE(MultiaddrHasPrivateIpv4Host("/ip4/10.0.0.1/tcp/18517/p2p/x"));
  EXPECT_TRUE(MultiaddrHasPrivateIpv4Host("/ip4/172.16.0.5/tcp/1/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host("/ip4/54.198.185.139/udp/4001/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host("/ip4/1.2.3.4/tcp/443/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host(""));
  // Global /ip6 is not a private IPv4 host (Wide PreferLocal / public-hop filters).
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host("/ip6/2001:db8::1/udp/4001/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host("/ip6/::1/udp/4001/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPrivateIpv4Host("/ip6/fe80::1/udp/4001/adp/1.0.0/p2p/x"));
}

TEST(MeshHopDialTest, MultiaddrHasPublicDialHost) {
  EXPECT_TRUE(MultiaddrHasPublicDialHost("/ip4/203.0.113.10/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_TRUE(MultiaddrHasPublicDialHost("/ip6/2001:db8::1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip4/10.0.0.1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip6/fe80::1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip6/fd12::1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip6/::/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost("/ip6/::1/udp/1/adp/1.0.0/p2p/x"));
  EXPECT_FALSE(MultiaddrHasPublicDialHost(""));
}

TEST(MeshHopDialTest, CircuitHopDialBookAllowsRegister) {
  // CollectDialableCircuitRelayIds / WarmBootstrapSeedSessions gate (dogfood sendto poison).
  EXPECT_TRUE(CircuitHopDialBookAllowsRegister("/ip4/203.0.113.10/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopDialBookAllowsRegister("/ip6/2001:db8::1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_FALSE(CircuitHopDialBookAllowsRegister("/ip4/10.0.0.1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_FALSE(CircuitHopDialBookAllowsRegister("/ip4/192.168.1.5/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_FALSE(CircuitHopDialBookAllowsRegister("/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_FALSE(CircuitHopDialBookAllowsRegister(""));
}

TEST(MeshHopDialTest, CircuitHopMultiaddrIsUdpDialable) {
  // Dogfood 084055: capability ingest Preferred=/ip4/0.0.0.0 → sendto. Skip only wildcard bind.
  EXPECT_FALSE(CircuitHopMultiaddrIsUdpDialable(""));
  EXPECT_FALSE(CircuitHopMultiaddrIsUdpDialable("/ip4/0.0.0.0/udp/53523/adp/1.0.0/p2p/hop"));
  EXPECT_FALSE(CircuitHopMultiaddrIsUdpDialable("/ip6/::/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip4/203.0.113.10/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip4/10.0.0.1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip4/192.168.1.5/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip6/2001:db8::1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/ip6/::1/udp/4001/adp/1.0.0/p2p/hop"));
  EXPECT_TRUE(CircuitHopMultiaddrIsUdpDialable("/memory/1/p2p/hop"));
}

TEST(MeshHopDialTest, PreferredDialMultiaddrPrefersGlobalIpv6) {
  const std::string lan = "/ip4/192.168.1.10/udp/4001/adp/1.0.0/p2p/12D3KooWLan";
  const std::string pub4 = "/ip4/203.0.113.10/udp/4001/adp/1.0.0/p2p/12D3KooWPub";
  const std::string v6 = "/ip6/2001:db8::9/udp/4001/adp/1.0.0/p2p/12D3KooWIpv6";
  EXPECT_EQ(PreferredDialMultiaddr({lan, pub4, v6}), v6);
  EXPECT_EQ(PreferredDialMultiaddr({lan, pub4}), pub4);
  EXPECT_EQ(PreferredDialMultiaddr({lan}), lan);
  EXPECT_TRUE(PreferredDialMultiaddr({}).empty());

  const auto worst_to_best = OrderDialMultiaddrsWorstToBest({v6, lan, pub4});
  ASSERT_EQ(worst_to_best.size(), 3u);
  EXPECT_EQ(worst_to_best.front(), lan);
  EXPECT_EQ(worst_to_best.back(), v6);
}

TEST(MeshHopDialTest, PreferLocalMediaHopPrependsAndDedupes) {
  MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.multiaddr = "/ip4/3.3.3.3/tcp/1/p2p/12D3KooWSeed";
  seed.affinity = MeshHopAffinity::OrgSeed;

  MeshHopCandidate self_dup;
  self_dup.peer_id = "12D3KooWSelf";
  self_dup.multiaddr = "/ip4/1.1.1.1/tcp/1/p2p/12D3KooWSelf";
  self_dup.affinity = MeshHopAffinity::Contact;

  EXPECT_EQ(PreferLocalMediaHop({seed}, "").size(), 1u);

  const std::string self_ma = "/ip4/10.0.0.1/tcp/18517/p2p/12D3KooWSelf";
  auto ranked = PreferLocalMediaHop({seed, self_dup}, "12D3KooWSelf", self_ma);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWSelf");
  EXPECT_TRUE(ranked[0].dialable);
  EXPECT_EQ(ranked[0].multiaddr, self_ma);
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWSeed");
}

} // namespace
} // namespace pp::mesh
