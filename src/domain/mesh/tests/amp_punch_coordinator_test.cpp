#include "domain/mesh/reachability/AmpPunchCoordinator.h"

#include "domain/mesh/tests/support/mesh_test_harness.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AmpPunchCoordinatorTest, NotStartedReturnsCodedFailure) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  AmpPunchCoordinator punch(harness->mgr_a(), {}, {});
  auto punched = punch.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::NotStarted);
}

TEST(AmpPunchCoordinatorTest, StartedRequiresIntroducerEndpoint) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  auto pump = [&]() { harness->PumpBoth(); };
  AmpPunchCoordinator punch(harness->mgr_a(), pump, {});
  punch.Start();
  auto punched = punch.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::EndpointNotRegistered);
  punch.Stop();
}

TEST(AmpPunchCoordinatorTest, SeedIntroducerColdPunchConnectsAToB) {
  auto created = pbr::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  // A and B must accept inbound for simultaneous dial (A026 dual-dial).
  harness->ep_a->SetAcceptEnabled(true);
  harness->ep_b->SetAcceptEnabled(true);

  // Topology: A↔I and B↔I only — no direct A↔B endpoints registered initially.
  // Introducer registers A/B under their PeerIds so ResolvePeerKey does not open a second ADP assoc.
  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_a, harness->ma_a)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_b, harness->ma_b)));

  auto pump = [&]() { harness->PumpAll(); };
  AmpPunchCoordinator punch_a(harness->mgr_a(), pump, {});
  AmpPunchCoordinator punch_i(harness->mgr_r(), pump, {});
  AmpPunchCoordinator punch_b(harness->mgr_b(), pump, {});
  punch_a.SetLocalCandidateAddrs({harness->ma_a});
  punch_i.SetLocalCandidateAddrs({harness->ma_r});
  punch_b.SetLocalCandidateAddrs({harness->ma_b});
  punch_a.Start();
  punch_i.Start();
  punch_b.Start();

  // Warm A→I and B→I. Introducer should see both via FindLinkByPeerId after handshake.
  bool a_ready = false;
  bool b_ready = false;
  harness->mgr_a().EnsureAssociation("introducer", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    a_ready = static_cast<bool>(r);
  });
  harness->mgr_b().EnsureAssociation("introducer", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    b_ready = static_cast<bool>(r);
  });
  harness->PumpUntil([&] { return a_ready && b_ready; }, 2000);
  ASSERT_TRUE(a_ready);
  ASSERT_TRUE(b_ready);
  ASSERT_NE(harness->mgr_r().FindLinkByPeerId(harness->peer_id_a), nullptr);
  ASSERT_NE(harness->mgr_r().FindLinkByPeerId(harness->peer_id_b), nullptr);
  ASSERT_TRUE(harness->mgr_a().IsConnected("introducer"));
  ASSERT_TRUE(harness->mgr_b().IsConnected("introducer"));
  ASSERT_FALSE(harness->mgr_a().FindLinkByPeerId(harness->peer_id_b) != nullptr &&
               harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b) > 0);

  auto punched = punch_a.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 3000);
  ASSERT_TRUE(static_cast<bool>(punched)) << punched.error().message;
  EXPECT_TRUE(punched->ok) << punched->error;
  EXPECT_FALSE(punched->winner_multiaddr.empty());

  harness->PumpUntil(
      [&] {
        return harness->mgr_a().FindLinkByPeerId(harness->peer_id_b) != nullptr &&
               harness->mgr_b().FindLinkByPeerId(harness->peer_id_a) != nullptr &&
               harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b) >= 1 &&
               harness->mgr_b().CountConnectedLinksForPeerId(harness->peer_id_a) >= 1;
      },
      5000);
  // A026: at most one Connected link per PeerId after dual-dial election.
  EXPECT_NE(harness->mgr_a().FindLinkByPeerId(harness->peer_id_b), nullptr);
  EXPECT_NE(harness->mgr_b().FindLinkByPeerId(harness->peer_id_a), nullptr);
  EXPECT_EQ(harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b), 1u);
  EXPECT_EQ(harness->mgr_b().CountConnectedLinksForPeerId(harness->peer_id_a), 1u);

  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}

} // namespace
} // namespace pbr
