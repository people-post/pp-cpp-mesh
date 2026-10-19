#include "domain/mesh/discovery/AmpDirectoryProtocol.h"

#include "common/SettledWait.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(AmpDirectoryProtocolTest, ListMeshNodesReturnsSeedSnapshot) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };

  AmpDirectoryProtocol seed(*harness->runtime_b, pump);
  AmpDirectoryProtocol client(*harness->runtime_a, pump);

  AmpDirectoryProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = harness->peer_id_b;
  seed.Configure(seed_cfg);

  MeshNodeHit published;
  published.relay_user_id = "relay-seed";
  published.account_id = "account:seed";
  published.nickname = "SeedNode";
  published.entity_kind = "mesh_node";
  published.seq = 7;
  published.capabilities.circuit_relay = true;
  published.capabilities.media_relay = true;
  published.capabilities.dht = true;
  published.capabilities.ledger_gateway = true;
  DirectoryEndpoint ep;
  ep.peer_id = harness->peer_id_b;
  ep.multiaddrs = {harness->ma_b};
  published.endpoints.push_back(std::move(ep));
  seed.SetNodesSnapshot({published});

  AmpDirectoryProtocolConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.query_peer_keys = {"seed"};
  client.Configure(client_cfg);

  seed.Start();
  client.Start();

  auto listed = client.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(listed)) << listed.error().message;
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ(listed->front().relay_user_id, "relay-seed");
  EXPECT_EQ(listed->front().account_id.value_or(""), "account:seed");
  EXPECT_EQ(listed->front().seq, 7);
  EXPECT_TRUE(listed->front().capabilities.ledger_gateway);
  ASSERT_FALSE(listed->front().endpoints.empty());
  EXPECT_EQ(listed->front().endpoints.front().peer_id, harness->peer_id_b);

  AmpDirectoryClient adapter(client);
  auto via_client = adapter.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(via_client)) << via_client.error().message;
  EXPECT_EQ(via_client->size(), 1u);
  EXPECT_FALSE(static_cast<bool>(adapter.SearchPeople("x")));

  client.Stop();
  seed.Stop();
}

TEST(AmpDirectoryProtocolTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  EXPECT_EQ(AmpDirectoryProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow")).GetCode(),
            AmpDirectoryProtocol::Err::Timeout);
  EXPECT_EQ(
      AmpDirectoryProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux")).GetCode(),
      AmpDirectoryProtocol::Err::ChannelFailed);
}

} // namespace
} // namespace pbr
