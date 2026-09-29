#include "domain/mesh/reachability/dial_back/client/DialBackClient.h"
#include "domain/mesh/reachability/dial_back/serve/DialBackServer.h"

#include "domain/mesh/reachability/dial_back/DialBackTypes.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(DialBackServerClientTest, ProbeRoundTripOk) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  DialBackServer seed(*harness->runtime_b);
  DialBackClient client(*harness->runtime_a, pump);
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

TEST(DialBackServerClientTest, ProbeNotStartedReturnsCodedFailure) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  DialBackClient client(*harness->runtime_a);
  auto probed = client.Probe("seed", {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(probed));
  EXPECT_EQ(probed.error().GetCode(), DialBackClient::Err::NotStarted);
}

TEST(DialBackServerClientTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  {
    const auto wrapped =
        DialBackClient::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::EndpointNotRegistered, "missing"));
    EXPECT_EQ(wrapped.GetCode(), DialBackClient::Err::EndpointNotRegistered);
    EXPECT_NE(wrapped.message.find("[link:"), std::string::npos);
  }
  {
    const auto wrapped = DialBackClient::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow"));
    EXPECT_EQ(wrapped.GetCode(), DialBackClient::Err::Timeout);
  }
  {
    const auto wrapped =
        DialBackClient::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux"));
    EXPECT_EQ(wrapped.GetCode(), DialBackClient::Err::ChannelFailed);
  }
  {
    const auto wrapped =
        DialBackClient::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::AssociationNotReady, "not ready"));
    EXPECT_EQ(wrapped.GetCode(), DialBackClient::Err::LinkFailed);
  }
}

TEST(DialBackServerClientTest, ProbeRejectsNonAdpTarget) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));

  auto pump = [&]() { harness->PumpBoth(); };
  DialBackServer seed(*harness->runtime_b);
  DialBackClient client(*harness->runtime_a, pump);
  seed.Start();
  client.Start();

  // ServeProbe filters to ADP targets on the requester's observed host before dialing anything
  // (see ProbeFiltersToObservedHostBeforeCappingTargetCount), so a non-ADP target never reaches
  // the dial walk at all; the filtered set is empty and the probe reports it as such.
  const std::string bad = "/ip4/203.0.113.1/tcp/39999/p2p/" + harness->peer_id_a;
  auto probed = client.Probe("seed", {bad}, 2000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_FALSE(probed->ok);
  EXPECT_NE(probed->error.find("no target_multiaddrs"), std::string::npos) << probed->error;

  client.Stop();
  seed.Stop();
}

TEST(DialBackServerClientTest, ProbeRejectsTargetNotMatchingObservedHost) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  DialBackServer seed(*harness->runtime_b);
  DialBackClient client(*harness->runtime_a, pump);
  seed.Start();
  client.Start();

  // A well-formed ADP target whose host is not the client's own observed connection: the seed
  // must not become an open "dial anywhere for anyone" relay for an arbitrary third party.
  // ServeProbe drops it during host filtering (before it ever reaches the dial walk), so the
  // filtered set is empty and the probe reports it as such (see ProbeFiltersToObservedHost...).
  const std::string third_party = "/ip4/203.0.113.9/udp/9999/adp/1.0.0/p2p/" + harness->peer_id_a;
  auto probed = client.Probe("seed", {third_party}, 2000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_FALSE(probed->ok);
  EXPECT_NE(probed->error.find("no target_multiaddrs"), std::string::npos) << probed->error;

  client.Stop();
  seed.Stop();
}

TEST(DialBackServerClientTest, ProbeFiltersToObservedHostBeforeCappingTargetCount) {
  // Regression: targets must be filtered to the requester's observed host BEFORE capping to
  // kMaxDialBackTargets (4). Capping first would let a requester pad the request with 4+
  // non-matching decoys ahead of the one real (matching) target and have that legitimate
  // target dropped before the host filter ever saw it.
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  DialBackServer seed(*harness->runtime_b);
  DialBackClient client(*harness->runtime_a, pump);
  seed.Start();
  client.Start();

  std::vector<std::string> targets;
  for (int i = 0; i < 4; ++i) {
    targets.push_back("/ip4/203.0.113." + std::to_string(9 + i) + "/udp/9999/adp/1.0.0/p2p/" + harness->peer_id_a);
  }
  targets.push_back(harness->ma_a); // the one target that matches the client's observed host

  auto probed = client.Probe("seed", targets, 8000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_TRUE(probed->ok) << probed->error;
  EXPECT_EQ(probed->dialed, harness->ma_a);

  client.Stop();
  seed.Stop();
}

pp::adp::IpEndpoint V6(std::initializer_list<uint8_t> bytes) {
  std::array<uint8_t, 16> addr{};
  size_t i = 0;
  for (const uint8_t b : bytes) {
    addr[i++] = b;
  }
  return pp::adp::IpEndpoint::V6(addr, 4001);
}

// Which hosts a seed dials for a requester: its own address (v4), its own /64 (v6 — a SLAAC
// privacy source still vouches for the stable advertised address), or across families only a
// public-routable target. Never a private / loopback / link-local third party.
TEST(DialBackTargetAllowedTest, OwnHostOwnPrefixOrPublicAcrossFamilies) {
  const auto observed_v4 = pp::adp::IpEndpoint::V4(198, 51, 7, 20, 40000);
  EXPECT_TRUE(DialBackTargetAllowed(pp::adp::IpEndpoint::V4(198, 51, 7, 20, 4001), observed_v4)) << "port ignored";
  EXPECT_FALSE(DialBackTargetAllowed(pp::adp::IpEndpoint::V4(198, 51, 7, 21, 4001), observed_v4));
  EXPECT_FALSE(DialBackTargetAllowed(pp::adp::IpEndpoint::V4(8, 8, 8, 8, 4001), observed_v4))
      << "another public v4 host is a third party";

  const auto observed_v6 = V6({0x20, 0x01, 0x48, 0x60, 0x12, 0x34, 0x56, 0x78, 0xaa, 0xbb, 0, 0, 0, 0, 0, 1});
  EXPECT_TRUE(DialBackTargetAllowed(V6({0x20, 0x01, 0x48, 0x60, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0, 0, 0, 0, 9}),
                                    observed_v6))
      << "same /64 (privacy source vs stable address)";
  EXPECT_FALSE(DialBackTargetAllowed(V6({0x20, 0x01, 0x48, 0x60, 0x12, 0x34, 0x56, 0x79, 0, 0, 0, 0, 0, 0, 0, 9}),
                                     observed_v6))
      << "another /64";

  // Across families: the global v6 targets reach ranks first, while the link runs over v4.
  EXPECT_TRUE(DialBackTargetAllowed(V6({0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88}),
                                    observed_v4));
  EXPECT_FALSE(DialBackTargetAllowed(V6({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), observed_v4))
      << "link-local";
  EXPECT_FALSE(DialBackTargetAllowed(V6({0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), observed_v4)) << "ULA";
  EXPECT_FALSE(DialBackTargetAllowed(pp::adp::IpEndpoint::V4(10, 0, 0, 1, 4001), observed_v6)) << "private v4";
  EXPECT_TRUE(DialBackTargetAllowed(pp::adp::IpEndpoint::V4(8, 8, 8, 8, 4001), observed_v6));
}

// Review regression: a client whose link to the seed runs over IPv4 still gets its global IPv6
// target dialed (it used to be filtered out, so the client looked unreachable over IPv6).
TEST(DialBackServerClientTest, GlobalV6TargetIsDialedOverAV4Link) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };
  DialBackServer seed(*harness->runtime_b);
  DialBackClient client(*harness->runtime_a, pump);
  seed.Start();
  client.Start();

  const std::string global_v6 = "/ip6/2001:4860:4860::8888/udp/9999/adp/1.0.0/p2p/" + harness->peer_id_a;
  auto probed = client.Probe("seed", {global_v6}, 2000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  // Nothing answers at that address in the harness, so the dial itself fails — but the seed tried
  // it (it was not dropped by the host filter as "no target_multiaddrs").
  EXPECT_EQ(probed->error.find("no target_multiaddrs"), std::string::npos) << probed->error;
  EXPECT_EQ(probed->dialed, global_v6);

  client.Stop();
  seed.Stop();
}

} // namespace
} // namespace pbr
