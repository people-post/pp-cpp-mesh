#include "base/mesh/reachability/AmpDialBackService.h"

#include "base/mesh/tests/support/mesh_test_harness.h"
#include "base/mesh/reachability/DialBackTypes.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(AmpDialBackServiceTest, ProbeRoundTripOk) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  AmpDialBackService seed(harness->mgr_b(), pump, {});
  AmpDialBackService client(harness->mgr_a(), pump, {});
  seed.Start();
  client.Start();

  // Seed dials client's advertised ADP listen (ma_a).
  auto probed = client.Probe("seed", {harness->ma_a}, 8000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_TRUE(probed->ok) << probed->error;
  EXPECT_EQ(probed->dialed, harness->ma_a);

  client.Stop();
  seed.Stop();
}

TEST(AmpDialBackServiceTest, ProbeRejectsNonAdpTarget) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));

  auto pump = [&]() { harness->PumpBoth(); };
  AmpDialBackService seed(harness->mgr_b(), pump, {});
  AmpDialBackService client(harness->mgr_a(), pump, {});
  seed.Start();
  client.Start();

  const std::string bad = "/ip4/203.0.113.1/tcp/39999/p2p/" + harness->peer_id_a;
  auto probed = client.Probe("seed", {bad}, 2000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_FALSE(probed->ok);
  EXPECT_NE(probed->error.find("ADP"), std::string::npos);

  client.Stop();
  seed.Stop();
}

} // namespace
} // namespace pbr
