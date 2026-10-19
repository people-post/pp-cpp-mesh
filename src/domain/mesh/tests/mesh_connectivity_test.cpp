#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/mesh/media_plane/MeshMediaRelay.h"
#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
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
TEST(MeshConnectivityTest, ListenBookMergesAndRegistersDirectEndpointsWithoutADialRegistry) {
  MeshConnectivity connectivity;
  std::vector<std::pair<std::string, std::string>> registered;
  MeshConnectivityDeps deps;
  deps.register_direct_endpoint = [&](const std::string& key, const std::string& ma) { registered.emplace_back(key, ma); };
  connectivity.SetDeps(std::move(deps));

  const std::string pub = Ma("203.0.113.7", 4001);
  // No runtime here: the owner step runs inline.
  std::string peer_id;
  connectivity.RegisterPeerListenMultiaddrs("account:bob", {pub}, [&peer_id](const std::string& id) { peer_id = id; });
  EXPECT_EQ(peer_id, kPeer);
  ASSERT_FALSE(registered.empty());
  EXPECT_EQ(registered.back().second, pub);
  EXPECT_EQ(connectivity.PeerListenBook()->at("account:bob"), std::vector<std::string>{pub});

  const std::string second = Ma("198.51.100.9", 4002);
  connectivity.RegisterPeerListenMultiaddrs("account:bob", {pub, second});
  const auto book = connectivity.PeerListenBook()->at("account:bob");
  EXPECT_EQ(book.size(), 2u) << "merged without duplicates";
}

TEST(MeshConnectivityTest, EmptyInputsRegisterNothing) {
  MeshConnectivity connectivity;
  std::string peer_id = "unset";
  connectivity.RegisterPeerListenMultiaddrs("", {Ma("203.0.113.7", 4001)}, [&peer_id](const std::string& id) { peer_id = id; });
  EXPECT_TRUE(peer_id.empty());
  peer_id = "unset";
  connectivity.RegisterPeerListenMultiaddrs("account:bob", {}, [&peer_id](const std::string& id) { peer_id = id; });
  EXPECT_TRUE(peer_id.empty());
  EXPECT_TRUE(connectivity.PeerListenBook()->empty());
}

// No mesh: nothing is wired, and every shared object reads as absent rather than dangling.
TEST(MeshConnectivityTest, WireWithoutAMeshLeavesNoSharedObjects) {
  MeshConnectivity connectivity;
  MeshMediaRelay media_relay(connectivity);
  connectivity.Wire();
  media_relay.Wire();
  const MediaRelayAttachPorts ports = media_relay.RelayAttachPorts();
  EXPECT_EQ(ports.relay, nullptr);
  EXPECT_EQ(ports.service_reach, nullptr);
  EXPECT_FALSE(media_relay.AmpRelayAvailable());
  EXPECT_FALSE(connectivity.TryEnsurePeerReachable("peer"));
}

// THREADING.md § Mesh threads, rule 1: the candidate policy is the app's, pushed from any thread; the
// mesh publishes it as is (the Amp IO side reads the snapshot) and keeps it across Wire.
TEST(MeshConnectivityTest, HopPolicyIsTheAppsPushedSnapshot) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  {
    MeshConnectivity connectivity;
    EXPECT_TRUE(connectivity.HopPolicy()->bootstrap_seeds.empty());
    MeshHopCandidate seed;
    seed.peer_id = "12D3KooWSeedA";
    MeshHopPolicy policy;
    policy.bootstrap_seeds = {seed};
    std::thread([&connectivity, policy]() { connectivity.SetHopPolicy(policy); }).join();
    ASSERT_EQ(connectivity.HopPolicy()->bootstrap_seeds.size(), 1u);
    EXPECT_EQ(connectivity.HopPolicy()->bootstrap_seeds.front().peer_id, "12D3KooWSeedA");

    connectivity.Wire();  // no mesh: nothing wired; the pushed policy stays
    ASSERT_EQ(connectivity.HopPolicy()->bootstrap_seeds.size(), 1u);
    connectivity.Clear();
    EXPECT_TRUE(connectivity.HopPolicy()->bootstrap_seeds.empty()) << "teardown drops it";
  }
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
