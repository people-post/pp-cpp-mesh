#include "base/mesh/discovery/MeshDirectoryCache.h"

#include "base/people/ContactTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MeshDirectoryCacheTest, NodesFromHitsFlattensEndpoints) {
  MeshNodeHit hit;
  hit.relay_user_id = "relay:abc";
  hit.capabilities.media_relay = true;
  hit.capabilities.circuit_relay = true;
  DirectoryEndpoint ep;
  ep.peer_id = "12D3KooWNode";
  ep.multiaddrs = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWNode"};
  hit.endpoints.push_back(ep);

  const auto nodes = MeshDirectoryNodesFromHits({hit});
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0].peer_id, "12D3KooWNode");
  EXPECT_TRUE(nodes[0].media_relay);
  EXPECT_TRUE(nodes[0].circuit_relay);
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

} // namespace
} // namespace pbr
