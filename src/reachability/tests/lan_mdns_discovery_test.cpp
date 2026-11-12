#include "mesh/reachability/LanMdnsDiscovery.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

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
            std::optional<std::string>("/ip4/192.168.1.42/udp/19001/adp/2/p2p/QmTestPeer"));
}

// An announcement says its wire version (TXT `wire=`): one of another version, or one that says none (a build
// before it), is not dialed, as an address of another wire version is not.
TEST(LanMdnsDiscoveryTest, AnAnnouncementOfAnotherWireOrNoneIsNotHeard) {
  const auto packet = LanMdnsDiscovery::EncodeAnnouncement("QmTestPeer", 19001, {"192.168.1.42"});
  const std::string ours = "wire=2";
  const auto at = std::search(packet.begin(), packet.end(), ours.begin(), ours.end());
  ASSERT_NE(at, packet.end()) << "the TXT record says our wire version";
  auto other = packet;
  other[static_cast<size_t>(at - packet.begin()) + ours.size() - 1] = '3';
  EXPECT_TRUE(LanMdnsDiscovery::ParseAnnouncement(other.data(), other.size()).empty());
  auto none = packet;
  none[static_cast<size_t>(at - packet.begin())] = 'x';
  EXPECT_TRUE(LanMdnsDiscovery::ParseAnnouncement(none.data(), none.size()).empty());
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

// pp-browser #596: a socket that stayed "readable" with nothing to read spun the discovery thread at a full core.
// Every empty read now waits longer, up to a second, and a run of them reopens the socket.
TEST(LanMdnsEmptyReadsTest, AnEmptyReadWaitsLongerAndARunReopens) {
  using std::chrono::milliseconds;
  LanMdnsEmptyReads reads;
  const std::vector<int64_t> waits = {50, 100, 200, 400, 800, 1000, 1000};
  for (size_t i = 0; i < waits.size(); ++i) {
    const auto next = reads.OnEmpty();
    EXPECT_EQ(next.wait, milliseconds(waits[i])) << "empty read " << i + 1;
    EXPECT_FALSE(next.reopen) << "empty read " << i + 1;
  }
  EXPECT_TRUE(reads.OnEmpty().reopen) << "the eighth empty read in a row reopens the socket";
  EXPECT_FALSE(reads.OnEmpty().reopen);
}

TEST(LanMdnsEmptyReadsTest, APacketEndsTheRun) {
  LanMdnsEmptyReads reads;
  for (int i = 0; i < 5; ++i) {
    (void)reads.OnEmpty();
  }
  reads.OnPacket();
  const auto next = reads.OnEmpty();
  EXPECT_EQ(next.wait, LanMdnsEmptyReads::kFirstWait);
  for (int i = 1; i < LanMdnsEmptyReads::kReopenEvery - 1; ++i) {
    EXPECT_FALSE(reads.OnEmpty().reopen);
  }
  EXPECT_TRUE(reads.OnEmpty().reopen) << "the count starts again after a packet";
}

} // namespace
} // namespace pp::mesh
