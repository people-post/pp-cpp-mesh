#include "base/mesh/dht/AmpDhtService.h"

#include "base/mesh/l4/shared/SettledWait.h"
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

  SettledWait<DhtFindPeerResult> wait;
  client.FindPeer(harness->peer_id_b, [&wait](Roe<DhtFindPeerResult> result) { wait.Finish(std::move(result)); });
  harness->PumpUntil([&]() { return wait.IsSettled(); }, 1000);

  auto found = wait.Wait(std::chrono::seconds(5), Error("find_peer timed out"));
  ASSERT_TRUE(static_cast<bool>(found)) << found.error().message;
  EXPECT_EQ(found->peer_id, harness->peer_id_b);
  EXPECT_FALSE(found->record.multiaddrs.empty());

  client.Stop();
  seed.Stop();
}

} // namespace
} // namespace pbr
