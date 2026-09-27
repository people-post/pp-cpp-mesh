#include "domain/mesh/media_plane/MeshMediaPlane.h"

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kPeer = "12D3KooWPeerListenBook";

std::string Ma(const std::string& host, int port) {
  return "/ip4/" + host + "/udp/" + std::to_string(port) + "/adp/1.0.0/p2p/" + kPeer;
}

// Without a mesh (no dial registry): the listen book still ranks and merges, the best dialable
// addr's PeerId comes back (calls learn account ↔ PeerId from it), and each addr goes to the
// delivery plane worst → best so the best one ends Preferred.
TEST(MeshMediaPlaneTest, ListenBookMergesAndRegistersDirectEndpointsWithoutADialRegistry) {
  MeshMediaPlane plane;
  std::vector<std::pair<std::string, std::string>> registered;
  MeshMediaPlaneDeps deps;
  deps.register_direct_endpoint = [&](const std::string& key, const std::string& ma) { registered.emplace_back(key, ma); };
  plane.SetDeps(std::move(deps));

  const std::string pub = Ma("203.0.113.7", 4001);
  const std::string peer_id = plane.RegisterPeerListenMultiaddrs("account:bob", {pub});
  EXPECT_EQ(peer_id, kPeer);
  ASSERT_FALSE(registered.empty());
  EXPECT_EQ(registered.back().second, pub);
  EXPECT_EQ(plane.PeerListenBook().at("account:bob"), std::vector<std::string>{pub});

  const std::string second = Ma("198.51.100.9", 4002);
  (void)plane.RegisterPeerListenMultiaddrs("account:bob", {pub, second});
  const auto book = plane.PeerListenBook().at("account:bob");
  EXPECT_EQ(book.size(), 2u) << "merged without duplicates";
}

TEST(MeshMediaPlaneTest, EmptyInputsRegisterNothing) {
  MeshMediaPlane plane;
  EXPECT_TRUE(plane.RegisterPeerListenMultiaddrs("", {Ma("203.0.113.7", 4001)}).empty());
  EXPECT_TRUE(plane.RegisterPeerListenMultiaddrs("account:bob", {}).empty());
  EXPECT_TRUE(plane.PeerListenBook().empty());
}

// No mesh: nothing is wired, and every shared object reads as absent rather than dangling.
TEST(MeshMediaPlaneTest, WireWithoutAMeshLeavesNoSharedObjects) {
  MeshMediaPlane plane;
  plane.Wire();
  const MediaRelayAttachPorts ports = plane.RelayAttachPorts();
  EXPECT_EQ(ports.relay, nullptr);
  EXPECT_EQ(ports.service_reach, nullptr);
  EXPECT_FALSE(plane.AmpRelayAvailable());
  EXPECT_FALSE(plane.TryEnsurePeerReachable("peer"));
}

} // namespace
} // namespace pbr
