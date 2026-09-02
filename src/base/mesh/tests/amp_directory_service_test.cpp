#include "base/mesh/discovery/AmpDirectoryService.h"
#include "domain/net/ServiceClientsImpl.h"

#include "common/SettledWait.h"
#include "base/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AmpDirectoryServiceTest, ListMeshNodesReturnsSeedSnapshot) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };

  AmpDirectoryService seed(harness->mgr_b(), pump, {});
  AmpDirectoryService client(harness->mgr_a(), pump, {});

  AmpDirectoryServiceConfig seed_cfg;
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

  AmpDirectoryServiceConfig client_cfg;
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

TEST(AmpDirectoryServiceTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  EXPECT_EQ(AmpDirectoryService::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow")).GetCode(),
            AmpDirectoryService::Err::Timeout);
  EXPECT_EQ(
      AmpDirectoryService::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux")).GetCode(),
      AmpDirectoryService::Err::ChannelFailed);
}

TEST(AmpDirectoryServiceTest, FailoverDirectoryFallsBackWhenAmpFails) {
  class FakeHttpDirectory : public IDirectoryClient {
  public:
    Roe<std::vector<DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("unused");
    }
    Roe<DirectoryHit> LookupRelayUser(const std::string&) override { return Error("unused"); }
    Roe<DirectoryHit> LookupByAccount(const std::string&) override { return Error("unused"); }
    Roe<std::vector<MeshNodeHit>> ListMeshNodes() override {
      MeshNodeHit hit;
      hit.relay_user_id = "from-http";
      hit.entity_kind = "mesh_node";
      return std::vector<MeshNodeHit>{hit};
    }
  };

  class FailingAmpDirectory : public IDirectoryClient {
  public:
    Roe<std::vector<DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<DirectoryHit> LookupRelayUser(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<DirectoryHit> LookupByAccount(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<std::vector<MeshNodeHit>> ListMeshNodes() override { return Error("amp unavailable"); }
  };

  std::vector<std::unique_ptr<IDirectoryClient>> backends;
  backends.push_back(std::make_unique<FailingAmpDirectory>());
  backends.push_back(std::make_unique<FakeHttpDirectory>());
  FailoverDirectoryClient failover(std::move(backends));
  auto nodes = failover.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(nodes)) << nodes.error().message;
  ASSERT_EQ(nodes->size(), 1u);
  EXPECT_EQ(nodes->front().relay_user_id, "from-http");
}

} // namespace
} // namespace pbr
