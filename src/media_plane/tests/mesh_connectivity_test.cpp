#include "mesh/connectivity/MeshConnectivity.h"
#include "mesh/media_plane/MeshMediaRelay.h"

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace pp::mesh {
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

// docs/THREADING.md, rule 1: the candidate policy is the app's, pushed from any thread; the
// mesh publishes it as is (the Amp IO side reads the snapshot) and keeps it across Wire.
TEST(MeshConnectivityTest, HopPolicyIsTheAppsPushedSnapshot) {
  {
    MeshConnectivity connectivity;
    EXPECT_TRUE(connectivity.HopPolicy()->Seeds().empty());
    MeshHopCandidate seed;
    seed.peer_id = "12D3KooWSeedA";
    MeshHopPolicy policy;
    seed.roles = kReachRoleSeed;
    policy.candidates = {seed};
    std::thread([&connectivity, policy]() { connectivity.SetHopPolicy(policy); }).join();
    ASSERT_EQ(connectivity.HopPolicy()->Seeds().size(), 1u);
    EXPECT_EQ(connectivity.HopPolicy()->Seeds().front().peer_id, "12D3KooWSeedA");

    connectivity.Wire();  // no mesh: nothing wired; the pushed policy stays
    ASSERT_EQ(connectivity.HopPolicy()->Seeds().size(), 1u);
    connectivity.Clear();
    EXPECT_TRUE(connectivity.HopPolicy()->Seeds().empty()) << "teardown drops it";
  }
}

// One list, each candidate with its roles (docs/PEER_REACH.md § 3.3): every role reads its view
// in the policy's order; introducers ask contacts first.
TEST(MeshConnectivityTest, EachRoleReadsItsViewOfTheOneList) {
  const auto hop = [](const char* id, MeshHopAffinity affinity, uint32_t roles) {
    MeshHopCandidate c;
    c.peer_id = id;
    c.affinity = affinity;
    c.roles = roles;
    return c;
  };
  MeshHopPolicy policy;
  policy.candidates = {
      hop("QmSeedA", MeshHopAffinity::OrgSeed, kReachRoleRendezvous | kReachRoleSeed | kReachRoleIntroducer),
      hop("QmFriend", MeshHopAffinity::Contact, kReachRoleRendezvous | kReachRoleIntroducer),
      hop("QmVolunteer", MeshHopAffinity::DirectoryNode, kReachRoleRendezvous | kReachRoleSeed),
      hop("QmStranger", MeshHopAffinity::DhtDiscovered, kReachRoleRendezvous),
      hop("QmMediaOnly", MeshHopAffinity::DirectoryNode, 0),
  };
  const auto ids = [](const std::vector<MeshHopCandidate>& hops) {
    std::vector<std::string> out;
    for (const auto& h : hops) {
      out.push_back(h.peer_id);
    }
    return out;
  };
  EXPECT_EQ(ids(policy.Rendezvous()), (std::vector<std::string>{"QmSeedA", "QmFriend", "QmVolunteer", "QmStranger"}));
  EXPECT_EQ(ids(policy.Seeds()), (std::vector<std::string>{"QmSeedA", "QmVolunteer"}));
  const MeshPunchIntroducers introducers = policy.Introducers();
  EXPECT_EQ(introducers.contact_peer_ids, std::vector<std::string>{"QmFriend"});
  EXPECT_EQ(introducers.seed_peer_ids, std::vector<std::string>{"QmSeedA"});
}

} // namespace
} // namespace pp::mesh
