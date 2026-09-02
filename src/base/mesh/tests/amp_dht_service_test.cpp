#include "base/mesh/dht/AmpDhtService.h"

#include "common/SettledWait.h"
#include "base/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AmpDhtServiceTest, FindPeerReturnsBootstrapRecord) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };

  AmpDhtService seed(harness->mgr_b(), pump, {});
  AmpDhtService client(harness->mgr_a(), pump, {});

  AmpDhtServiceConfig seed_cfg;
  seed_cfg.local_peer_id = harness->peer_id_b;
  seed_cfg.listen_multiaddrs = {harness->ma_b};
  seed_cfg.device_signing_secret = harness->bob.ml_dsa_secret_key;
  seed_cfg.device_signing_public = harness->bob.ml_dsa_public_key;
  seed_cfg.participate = true;
  seed_cfg.publish_media_relay = true;
  seed_cfg.publish_circuit_relay = true;
  seed.Configure(seed_cfg);

  AmpDhtServiceConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.listen_multiaddrs = {harness->ma_a};
  client_cfg.device_signing_secret = harness->alice.ml_dsa_secret_key;
  client_cfg.device_signing_public = harness->alice.ml_dsa_public_key;
  client_cfg.query_peer_keys = {"seed"};
  client_cfg.participate = false;
  client.Configure(client_cfg);

  seed.Start();
  client.Start();
  seed.Tick();

  SettledWait<DhtFindPeerResult, AmpDhtService::Failure> wait;
  client.FindPeer(harness->peer_id_b,
                  [&wait](AmpDhtService::FindPeerRoe result) { wait.Finish(std::move(result)); });
  harness->PumpUntil([&]() { return wait.IsSettled(); }, 1000);

  auto found =
      wait.Wait(std::chrono::seconds(5), AmpDhtService::Failure::Of(AmpDhtService::Err::Timeout, "find_peer timed out"));
  ASSERT_TRUE(static_cast<bool>(found)) << found.error().message;
  EXPECT_EQ(found->peer_id, harness->peer_id_b);
  EXPECT_FALSE(found->record.multiaddrs.empty());
  ASSERT_TRUE(found->record.capabilities.has_value());
  EXPECT_TRUE(found->record.capabilities->media_relay);
  EXPECT_TRUE(found->record.capabilities->circuit_relay);

  client.Stop();
  seed.Stop();
}

/** Lab acceptance: two participating Nodes discover each other's ADP addrs (no Brief HTTP). */
TEST(AmpDhtServiceTest, MutualDiscoverViaStoreAndWarmFindPeer) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint(harness->peer_id_b, harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint(harness->peer_id_a, harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };

  AmpDhtService node_a(harness->mgr_a(), pump, {});
  AmpDhtService node_b(harness->mgr_b(), pump, {});

  AmpDhtServiceConfig cfg_a;
  cfg_a.local_peer_id = harness->peer_id_a;
  cfg_a.listen_multiaddrs = {harness->ma_a};
  cfg_a.device_signing_secret = harness->alice.ml_dsa_secret_key;
  cfg_a.device_signing_public = harness->alice.ml_dsa_public_key;
  cfg_a.query_peer_keys = {harness->peer_id_b};
  cfg_a.participate = true;
  node_a.Configure(cfg_a);

  AmpDhtServiceConfig cfg_b;
  cfg_b.local_peer_id = harness->peer_id_b;
  cfg_b.listen_multiaddrs = {harness->ma_b};
  cfg_b.device_signing_secret = harness->bob.ml_dsa_secret_key;
  cfg_b.device_signing_public = harness->bob.ml_dsa_public_key;
  cfg_b.query_peer_keys = {harness->peer_id_a};
  cfg_b.participate = true;
  node_b.Configure(cfg_b);

  node_a.Start();
  node_b.Start();
  node_a.Tick();
  node_b.Tick();

  harness->PumpUntil(
      [&]() {
        return node_a.LocalRecord(harness->peer_id_b).has_value() &&
               node_b.LocalRecord(harness->peer_id_a).has_value();
      },
      2000);

  auto a_has_b = node_a.LocalRecord(harness->peer_id_b);
  auto b_has_a = node_b.LocalRecord(harness->peer_id_a);
  ASSERT_TRUE(a_has_b.has_value()) << "node A missing B's DHT record";
  ASSERT_TRUE(b_has_a.has_value()) << "node B missing A's DHT record";
  EXPECT_FALSE(a_has_b->multiaddrs.empty());
  EXPECT_FALSE(b_has_a->multiaddrs.empty());
  EXPECT_NE(a_has_b->multiaddrs.front().find("/adp/"), std::string::npos);
  EXPECT_NE(b_has_a->multiaddrs.front().find("/adp/"), std::string::npos);

  node_a.Stop();
  node_b.Stop();
}

TEST(AmpDhtServiceTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  EXPECT_EQ(AmpDhtService::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow")).GetCode(),
            AmpDhtService::Err::Timeout);
  EXPECT_EQ(AmpDhtService::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux")).GetCode(),
            AmpDhtService::Err::ChannelFailed);
  const auto wrapped =
      AmpDhtService::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::AssociationNotReady, "not ready"));
  EXPECT_EQ(wrapped.GetCode(), AmpDhtService::Err::LinkFailed);
  EXPECT_NE(wrapped.message.find("[link:"), std::string::npos);
}

} // namespace
} // namespace pbr
