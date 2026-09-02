#include "base/mesh/discovery/MeshDirectoryCache.h"
#include "base/mesh/discovery/NameDirectory.h"

#include "base/people/ContactTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class FakeDirectoryClient : public IDirectoryClient {
public:
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    return std::vector<DirectoryHit>{};
  }
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override {
    if (person.hit_id != relay_user_id) {
      return Error("not found");
    }
    return person;
  }
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override {
    if (!person.account_id || *person.account_id != account_id) {
      return Error("not found");
    }
    return person;
  }
  Roe<std::vector<MeshNodeHit>> ListMeshNodes() override { return mesh_nodes; }

  DirectoryHit person;
  std::vector<MeshNodeHit> mesh_nodes;
};

TEST(MeshDirectoryCacheTest, NodesFromHitsPreservesNameFields) {
  MeshNodeHit hit;
  hit.relay_user_id = "relay:abc";
  hit.account_id = "account:org";
  hit.nickname = "Org Node";
  hit.entity_kind = "mesh_node";
  hit.seq = 7;
  hit.expires_at = "2099-01-01T00:00:00Z";
  hit.capabilities.media_relay = true;
  hit.capabilities.circuit_relay = true;
  hit.capabilities.dht = true;
  hit.capabilities.ledger_gateway = true;
  DirectoryEndpoint ep;
  ep.peer_id = "12D3KooWNode";
  ep.multiaddrs = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWNode"};
  hit.endpoints.push_back(ep);

  const auto nodes = MeshDirectoryNodesFromHits({hit});
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0].peer_id, "12D3KooWNode");
  EXPECT_EQ(nodes[0].account_id, "account:org");
  EXPECT_EQ(nodes[0].nickname, "Org Node");
  EXPECT_EQ(nodes[0].entity_kind, "mesh_node");
  EXPECT_EQ(nodes[0].seq, 7);
  EXPECT_EQ(nodes[0].expires_at, "2099-01-01T00:00:00Z");
  EXPECT_TRUE(nodes[0].media_relay);
  EXPECT_TRUE(nodes[0].circuit_relay);
  EXPECT_TRUE(nodes[0].dht);
  EXPECT_TRUE(nodes[0].ledger_gateway);
  ASSERT_EQ(nodes[0].multiaddrs.size(), 1u);
}

TEST(MeshDirectoryCacheTest, SnapshotEmptyBeforeRefresh) {
  MeshDirectoryCache cache([]() -> Roe<std::vector<MeshDirectoryNode>> {
    MeshDirectoryNode node;
    node.peer_id = "12D3KooWNode";
    return std::vector<MeshDirectoryNode>{node};
  });
  EXPECT_TRUE(cache.Snapshot().empty());
}

TEST(NameDirectoryTest, ListServiceMapsMeshNodes) {
  FakeDirectoryClient fake;
  MeshNodeHit hit;
  hit.relay_user_id = "relay:node";
  hit.account_id = "account:node";
  hit.seq = 3;
  DirectoryEndpoint ep;
  ep.peer_id = "12D3KooWList";
  ep.multiaddrs = {"/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/12D3KooWList"};
  hit.endpoints.push_back(ep);
  fake.mesh_nodes.push_back(hit);

  DirectoryClientNameDirectory names(fake);
  auto records = names.ListService("mesh_node");
  ASSERT_TRUE(static_cast<bool>(records));
  ASSERT_EQ(records->size(), 1u);
  EXPECT_EQ(records->front().account_id, "account:node");
  EXPECT_EQ(records->front().peer_id, "12D3KooWList");
  EXPECT_EQ(records->front().seq, 3);
  EXPECT_EQ(records->front().entity_kind, "mesh_node");
}

TEST(NameDirectoryTest, ResolveByAccount) {
  FakeDirectoryClient fake;
  fake.person.hit_id = "relay:alice";
  fake.person.account_id = "account:alice";
  fake.person.nickname = "Alice";
  fake.person.entity_kind = "person";
  fake.person.seq = 2;
  DirectoryEndpoint ep;
  ep.peer_id = "12D3KooWAlice";
  fake.person.endpoints.push_back(ep);

  DirectoryClientNameDirectory names(fake);
  auto record = names.Resolve("account:alice");
  ASSERT_TRUE(static_cast<bool>(record));
  EXPECT_EQ(record->name, "account:alice");
  EXPECT_EQ(record->peer_id, "12D3KooWAlice");
  EXPECT_EQ(record->entity_kind, "person");
  EXPECT_EQ(record->seq, 2);
}

} // namespace
} // namespace pbr
