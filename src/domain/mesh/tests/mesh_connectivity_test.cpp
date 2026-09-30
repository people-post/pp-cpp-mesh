#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/mesh/media_plane/MeshMediaRelay.h"
#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>
#include <atomic>
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

// thread-ownership t3-2b: candidate policy is evaluated on the Connectivity owner — never on the
// Amp IO strand — and the IO side reads the published snapshot.
TEST(MeshConnectivityTest, HopPolicyIsEvaluatedOnTheConnectivityOwnerAndPublished) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  {
    MeshConnectivity connectivity;
    std::atomic<int> evaluations{0};
    std::atomic<bool> off_owner{false};
    std::string seed = "12D3KooWSeedA";
    MeshConnectivityDeps deps;
    deps.bootstrap_seeds = [&]() {
      ++evaluations;
      if (!AppRuntime::CurrentlyOn(OwnerThreadId::Connectivity)) {
        off_owner = true;
      }
      MeshHopCandidate hop;
      hop.peer_id = seed;
      return std::vector<MeshHopCandidate>{hop};
    };
    connectivity.SetDeps(std::move(deps));
    connectivity.Wire();  // no mesh: nothing wired, but the policy is read once
    ASSERT_EQ(connectivity.HopPolicy()->bootstrap_seeds.size(), 1u);
    EXPECT_EQ(connectivity.HopPolicy()->bootstrap_seeds.front().peer_id, "12D3KooWSeedA");

    seed = "12D3KooWSeedB";
    const int before = evaluations.load();
    connectivity.RefreshHopPolicy();
    EXPECT_EQ(evaluations.load(), before) << "posted to the owner, not run by the caller";
    EXPECT_EQ(connectivity.HopPolicy()->bootstrap_seeds.front().peer_id, "12D3KooWSeedA");
    AppRuntime::RunOwnerTasks(OwnerThreadId::Connectivity);
    EXPECT_EQ(connectivity.HopPolicy()->bootstrap_seeds.front().peer_id, "12D3KooWSeedB");
    EXPECT_FALSE(off_owner.load()) << "providers only ever run on the Connectivity owner";
  }
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
