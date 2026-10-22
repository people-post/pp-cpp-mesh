#include "mesh/reachability/LanMdnsDiscovery.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

TEST(LanMdnsDiscoveryTest, TheServiceRunsOverUdp) {
  // DNS-SD: `_tcp` only for services reached over TCP; Amp runs over ADP (UDP).
  EXPECT_STREQ(kLanMdnsServiceType, "_pp-mesh._udp.local");
}

TEST(LanMdnsDiscoveryTest, EncodeDecodeDnsName) {
  auto encoded = LanMdnsDiscovery::EncodeDnsName(kLanMdnsServiceType);
  ASSERT_TRUE(encoded);
  size_t next = 0;
  auto decoded = LanMdnsDiscovery::DecodeDnsName(*encoded, 0, &next);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, kLanMdnsServiceType);
  EXPECT_EQ(next, encoded->size());
}

// What a peer announces is what the browser hears: its PeerId, address and Amp UDP port — a
// dialable ADP multiaddr.
TEST(LanMdnsDiscoveryTest, AnAnnouncementGivesTheAmpAddress) {
  const auto packet = LanMdnsDiscovery::EncodeAnnouncement("QmTestPeer", 19001, {"192.168.1.42"});
  ASSERT_FALSE(packet.empty());
  const auto peers = LanMdnsDiscovery::ParseAnnouncement(packet.data(), packet.size());
  ASSERT_EQ(peers.size(), 1u);
  EXPECT_EQ(peers[0].peer_id_base58, "QmTestPeer");
  EXPECT_EQ(peers[0].host_ip, "192.168.1.42");
  EXPECT_EQ(peers[0].amp_udp_port, 19001);
  EXPECT_EQ(LanMdnsDiscovery::BuildAdpMultiaddr(peers[0]),
            std::optional<std::string>("/ip4/192.168.1.42/udp/19001/adp/1.0.0/p2p/QmTestPeer"));
}

TEST(LanMdnsDiscoveryTest, NothingToAnnounceWithoutAPortOrAnAddress) {
  EXPECT_TRUE(LanMdnsDiscovery::EncodeAnnouncement("QmTestPeer", 0, {"192.168.1.42"}).empty());
  EXPECT_TRUE(LanMdnsDiscovery::EncodeAnnouncement("QmTestPeer", 19001, {}).empty());
  EXPECT_TRUE(LanMdnsDiscovery::EncodeAnnouncement("", 19001, {"192.168.1.42"}).empty());
}

TEST(LanMdnsDiscoveryTest, BuildAdpMultiaddrNeedsAPort) {
  LanMdnsDiscoveredPeer peer;
  peer.peer_id_base58 = "QmTestPeer";
  peer.host_ip = "192.168.1.42";
  EXPECT_FALSE(LanMdnsDiscovery::BuildAdpMultiaddr(peer).has_value());
}

} // namespace
} // namespace pp::mesh
