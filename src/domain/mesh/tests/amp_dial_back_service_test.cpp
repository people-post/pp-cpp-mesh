#include "domain/mesh/reachability/AmpDialBackProtocol.h"

#include "domain/mesh/reachability/DialBackTypes.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(AmpDialBackProtocolTest, ProbeRoundTripOk) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  AmpDialBackProtocol seed(harness->mgr_b(), pump, {});
  AmpDialBackProtocol client(harness->mgr_a(), pump, {});
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

TEST(AmpDialBackProtocolTest, ProbeNotStartedReturnsCodedFailure) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  AmpDialBackProtocol client(harness->mgr_a(), {}, {});
  auto probed = client.Probe("seed", {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(probed));
  EXPECT_EQ(probed.error().GetCode(), AmpDialBackProtocol::Err::NotStarted);
}

TEST(AmpDialBackProtocolTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  {
    const auto wrapped =
        AmpDialBackProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::EndpointNotRegistered, "missing"));
    EXPECT_EQ(wrapped.GetCode(), AmpDialBackProtocol::Err::EndpointNotRegistered);
    EXPECT_NE(wrapped.message.find("[link:"), std::string::npos);
  }
  {
    const auto wrapped = AmpDialBackProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow"));
    EXPECT_EQ(wrapped.GetCode(), AmpDialBackProtocol::Err::Timeout);
  }
  {
    const auto wrapped =
        AmpDialBackProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux"));
    EXPECT_EQ(wrapped.GetCode(), AmpDialBackProtocol::Err::ChannelFailed);
  }
  {
    const auto wrapped =
        AmpDialBackProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::AssociationNotReady, "not ready"));
    EXPECT_EQ(wrapped.GetCode(), AmpDialBackProtocol::Err::LinkFailed);
  }
}

TEST(AmpDialBackProtocolTest, ProbeRejectsNonAdpTarget) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));

  auto pump = [&]() { harness->PumpBoth(); };
  AmpDialBackProtocol seed(harness->mgr_b(), pump, {});
  AmpDialBackProtocol client(harness->mgr_a(), pump, {});
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
