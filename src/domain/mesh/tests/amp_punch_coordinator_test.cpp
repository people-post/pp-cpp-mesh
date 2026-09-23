#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchBurst.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "domain/mesh/reachability/PunchTypes.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/link/PeerLink.h"
#include "common/SettledWait.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

namespace pbr {
namespace {

/** Shared prelude for SyncWindowExpiry stage tests: A↔R↔B associated, no A↔B. */
struct PunchExpiryStageFixture {
  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness;
  std::string blackhole_a;
  std::string blackhole_b;

  static testing::AssertionResult Create(PunchExpiryStageFixture* out) {
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    if (!created) {
      return testing::AssertionFailure() << created.error().message;
    }
    out->harness = std::move(*created);
    out->harness->ep_a->SetAcceptEnabled(true);
    out->harness->ep_b->SetAcceptEnabled(true);
    if (!out->harness->mgr_a().RegisterEndpoint("introducer", out->harness->ma_r) ||
        !out->harness->mgr_b().RegisterEndpoint("introducer", out->harness->ma_r) ||
        !out->harness->mgr_r().RegisterEndpoint(out->harness->peer_id_a, out->harness->ma_a) ||
        !out->harness->mgr_r().RegisterEndpoint(out->harness->peer_id_b, out->harness->ma_b)) {
      return testing::AssertionFailure() << "RegisterEndpoint failed";
    }
    out->blackhole_a = "/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/" + out->harness->peer_id_a;
    out->blackhole_b = "/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/" + out->harness->peer_id_b;
    return testing::AssertionSuccess();
  }

  void PumpAll() { harness->PumpAll(); }

  bool AssocAAndBToIntroducer() {
    bool a_ready = false;
    bool b_ready = false;
    harness->mgr_a().EnsureAssociation("introducer", [&](pp::amp::PeerLinkManager::LinkRoe r) {
      a_ready = static_cast<bool>(r);
    });
    harness->mgr_b().EnsureAssociation("introducer", [&](pp::amp::PeerLinkManager::LinkRoe r) {
      b_ready = static_cast<bool>(r);
    });
    harness->PumpUntil([&] { return a_ready && b_ready; }, 2000);
    return a_ready && b_ready;
  }
};

/** MeshRuntime::PostToIo — punch frame handlers enqueue strand work; PumpAll drains it. */
AmpPunchCoordinator::IoPost PostIo(pp::amp::MeshRuntime& rt) {
  return [&rt](std::function<void()> task) { rt.PostToIo(std::move(task)); };
}

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
  AmpPunchCoordinator punch_a(harness->mgr_a(), pump, {}, PostIo(*harness->runtime_a));
  AmpPunchCoordinator punch_i(harness->mgr_r(), pump, {}, PostIo(*harness->runtime_r));
  AmpPunchCoordinator punch_b(harness->mgr_b(), pump, {}, PostIo(*harness->runtime_b));
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
  // L3.25b: initiator address-book upsert — SoftMigrate IsDialable(peer_id) on the dialer.
  EXPECT_TRUE(harness->mgr_a().GetLinkSnapshot(harness->peer_id_b).has_endpoint);
  // Target may already be Connected via inbound A026; book upsert is best-effort there.
  if (harness->mgr_b().GetLinkSnapshot(harness->peer_id_a).has_endpoint) {
    EXPECT_TRUE(harness->mgr_b().PreferredMultiaddr(harness->peer_id_a).has_value());
  }

  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}


TEST(AmpPunchCoordinatorTest, ContactIntroducerColdPunchConnectsAToB) {
  auto created = pbr::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  harness->ep_a->SetAcceptEnabled(true);
  harness->ep_b->SetAcceptEnabled(true);

  // Contact-style introducer: register I under its PeerId (not a seed alias).
  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint(harness->peer_id_r, harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint(harness->peer_id_r, harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_a, harness->ma_a)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_b, harness->ma_b)));

  auto pump = [&]() { harness->PumpAll(); };
  AmpPunchCoordinator punch_a(harness->mgr_a(), pump, {}, PostIo(*harness->runtime_a));
  AmpPunchCoordinator punch_i(harness->mgr_r(), pump, {}, PostIo(*harness->runtime_r));
  AmpPunchCoordinator punch_b(harness->mgr_b(), pump, {}, PostIo(*harness->runtime_b));
  punch_a.SetLocalCandidateAddrs({harness->ma_a});
  punch_i.SetLocalCandidateAddrs({harness->ma_r});
  punch_b.SetLocalCandidateAddrs({harness->ma_b});
  punch_a.Start();
  punch_i.Start();
  punch_b.Start();

  bool a_ready = false;
  bool b_ready = false;
  harness->mgr_a().EnsureAssociation(harness->peer_id_r, [&](pp::amp::PeerLinkManager::LinkRoe r) {
    a_ready = static_cast<bool>(r);
  });
  harness->mgr_b().EnsureAssociation(harness->peer_id_r, [&](pp::amp::PeerLinkManager::LinkRoe r) {
    b_ready = static_cast<bool>(r);
  });
  harness->PumpUntil([&] { return a_ready && b_ready; }, 2000);
  ASSERT_TRUE(a_ready);
  ASSERT_TRUE(b_ready);

  const auto contact_ids = std::vector<std::string>{harness->peer_id_r};
  const auto seed_ids = std::vector<std::string>{};
  auto intro = PickPunchIntroducer(
      contact_ids, seed_ids, harness->peer_id_b,
      [&](const std::string& id) { return harness->mgr_a().GetLinkSnapshot(id).has_endpoint; },
      [&](const std::string& id) { return harness->mgr_a().IsConnected(id); });
  ASSERT_TRUE(intro.has_value());
  EXPECT_EQ(*intro, harness->peer_id_r);

  auto punched = punch_a.TryColdPunch(*intro, harness->peer_id_b, {harness->ma_a}, 3000);
  ASSERT_TRUE(static_cast<bool>(punched)) << punched.error().message;
  EXPECT_TRUE(punched->ok) << punched->error;
  EXPECT_TRUE(harness->mgr_a().GetLinkSnapshot(harness->peer_id_b).has_endpoint);

  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}

/**
 * L3.25 gap: simultaneous dual-dial (A026) under ACP — both sides accept inbound;
 * first authenticated PeerLink wins; loser burst aliases must not leave a second
 * Connected Session for the same PeerId (A027 parent-only drop).
 */
TEST(AmpPunchCoordinatorTest, DualDialRaceElectsSingleConnectedSession) {
  auto created = pbr::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  harness->ep_a->SetAcceptEnabled(true);
  harness->ep_b->SetAcceptEnabled(true);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_a, harness->ma_a)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_b, harness->ma_b)));

  auto pump = [&]() { harness->PumpAll(); };
  AmpPunchCoordinator punch_a(harness->mgr_a(), pump, {}, PostIo(*harness->runtime_a));
  AmpPunchCoordinator punch_i(harness->mgr_r(), pump, {}, PostIo(*harness->runtime_r));
  AmpPunchCoordinator punch_b(harness->mgr_b(), pump, {}, PostIo(*harness->runtime_b));
  punch_a.SetLocalCandidateAddrs({harness->ma_a});
  punch_i.SetLocalCandidateAddrs({harness->ma_r});
  punch_b.SetLocalCandidateAddrs({harness->ma_b});
  punch_a.Start();
  punch_i.Start();
  punch_b.Start();

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

  auto punched = punch_a.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 3000);
  ASSERT_TRUE(static_cast<bool>(punched)) << punched.error().message;
  EXPECT_TRUE(punched->ok) << punched->error;

  harness->PumpUntil(
      [&] {
        return harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b) >= 1 &&
               harness->mgr_b().CountConnectedLinksForPeerId(harness->peer_id_a) >= 1;
      },
      5000);
  // Extra pumps so A026 ScheduleDropLink can retire the dual-dial loser.
  for (int i = 0; i < 40; ++i) {
    harness->PumpAll();
  }

  EXPECT_EQ(harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b), 1u);
  EXPECT_EQ(harness->mgr_b().CountConnectedLinksForPeerId(harness->peer_id_a), 1u);

  // Winner may live under peer_id or a provisional punch:burst:* key after A026 elect —
  // only one Connected Session per PeerId either way.
  auto* link_a = harness->mgr_a().FindLinkByPeerId(harness->peer_id_b);
  auto* link_b = harness->mgr_b().FindLinkByPeerId(harness->peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);
  EXPECT_EQ(link_a->Phase(), pp::amp::PeerLinkPhase::Connected);
  EXPECT_EQ(link_b->Phase(), pp::amp::PeerLinkPhase::Connected);

  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}

/**
 * Staged coverage for SyncWindowExpiry (Windows SEH bisect). Keep the end-to-end test below.
 *
 * 1 AssocAAndBToIntroducerOnly
 * 2 PunchCoordinatorsStartWithBlackholeAddrs
 * 3 OpenPunchChannelToIntroducer
 * 4 IntroducerOpensPunchChannelToTarget
 * 5 RegisterBlackholePeerAddrsWithoutBurst
 * 6 BurstDialBlackholeAlone (no mux callback on stack)
 * 7 BurstDialAfterDeferredDrain (PostStrand-shaped)
 * 8 SyncWindowExpiryReturnsPunchFailed (full TryColdPunch)
 */

TEST(AmpPunchCoordinatorTest, Stage1_AssocAAndBToIntroducerOnly) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());
  EXPECT_TRUE(fx.harness->mgr_a().IsConnected("introducer"));
  EXPECT_TRUE(fx.harness->mgr_b().IsConnected("introducer"));
  EXPECT_FALSE(fx.harness->mgr_a().IsConnected(fx.harness->peer_id_b));
  EXPECT_EQ(fx.harness->mgr_a().CountConnectedLinksForPeerId(fx.harness->peer_id_b), 0u);
}

TEST(AmpPunchCoordinatorTest, Stage2_PunchCoordinatorsStartWithBlackholeAddrs) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  auto pump = [&]() { fx.PumpAll(); };
  AmpPunchCoordinator punch_a(fx.harness->mgr_a(), pump, {}, PostIo(*fx.harness->runtime_a));
  AmpPunchCoordinator punch_i(fx.harness->mgr_r(), pump, {}, PostIo(*fx.harness->runtime_r));
  AmpPunchCoordinator punch_b(fx.harness->mgr_b(), pump, {}, PostIo(*fx.harness->runtime_b));
  punch_a.SetLocalCandidateAddrs({fx.blackhole_a});
  punch_i.SetLocalCandidateAddrs({fx.harness->ma_r});
  punch_b.SetLocalCandidateAddrs({fx.blackhole_b});
  punch_a.Start();
  punch_i.Start();
  punch_b.Start();
  EXPECT_TRUE(punch_a.IsStarted());
  EXPECT_TRUE(punch_i.IsStarted());
  EXPECT_TRUE(punch_b.IsStarted());
  ASSERT_EQ(punch_a.LocalCandidateAddrs().size(), 1u);
  EXPECT_EQ(punch_a.LocalCandidateAddrs().front(), fx.blackhole_a);
  ASSERT_EQ(punch_b.LocalCandidateAddrs().size(), 1u);
  EXPECT_EQ(punch_b.LocalCandidateAddrs().front(), fx.blackhole_b);
  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}

TEST(AmpPunchCoordinatorTest, Stage3_OpenPunchChannelToIntroducer) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  auto pump = [&]() { fx.PumpAll(); };
  AmpPunchCoordinator punch_i(fx.harness->mgr_r(), pump, {}, PostIo(*fx.harness->runtime_r));
  punch_i.Start();

  SettledWait<uint32_t, Error> wait;
  auto policy = pp::amp::ControlJsonChannelPolicy(std::chrono::milliseconds{2000});
  policy.read_once = false;
  fx.harness->mgr_a().OpenChannel(
      "introducer", kAmpPunchProtocolId, policy,
      [wait](pp::amp::PeerLinkManager::ChannelRoe ch) {
        if (ch) {
          wait.Finish(*ch);
        } else {
          wait.Finish(Error(ch.error().message));
        }
      });
  fx.harness->PumpUntil([&] { return wait.IsSettled(); }, 2000);
  auto opened = wait.Wait(std::chrono::milliseconds(1), Error("punch channel open timed out"));
  ASSERT_TRUE(static_cast<bool>(opened)) << opened.error().message;
  EXPECT_NE(*opened, 0u);
  punch_i.Stop();
}

TEST(AmpPunchCoordinatorTest, Stage4_IntroducerOpensPunchChannelToTarget) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  auto pump = [&]() { fx.PumpAll(); };
  AmpPunchCoordinator punch_b(fx.harness->mgr_b(), pump, {}, PostIo(*fx.harness->runtime_b));
  punch_b.SetLocalCandidateAddrs({fx.blackhole_b});
  punch_b.Start();

  SettledWait<uint32_t, Error> wait;
  auto policy = pp::amp::ControlJsonChannelPolicy(std::chrono::milliseconds{2000});
  policy.read_once = false;
  fx.harness->mgr_r().OpenChannel(
      fx.harness->peer_id_b, kAmpPunchProtocolId, policy,
      [wait](pp::amp::PeerLinkManager::ChannelRoe ch) {
        if (ch) {
          wait.Finish(*ch);
        } else {
          wait.Finish(Error(ch.error().message));
        }
      });
  fx.harness->PumpUntil([&] { return wait.IsSettled(); }, 2000);
  auto opened = wait.Wait(std::chrono::milliseconds(1), Error("introducer→target punch channel timed out"));
  ASSERT_TRUE(static_cast<bool>(opened)) << opened.error().message;
  EXPECT_NE(*opened, 0u);
  punch_b.Stop();
}

TEST(AmpPunchCoordinatorTest, Stage5_RegisterBlackholePeerAddrsWithoutBurst) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  // Mirror sync-handler bookkeeping: RegisterEndpoint under PeerId, no dial yet.
  ASSERT_TRUE(static_cast<bool>(
      fx.harness->mgr_a().RegisterEndpoint(fx.harness->peer_id_b, fx.blackhole_b)));
  EXPECT_TRUE(fx.harness->mgr_a().GetLinkSnapshot(fx.harness->peer_id_b).has_endpoint);
  auto pref = fx.harness->mgr_a().PreferredMultiaddr(fx.harness->peer_id_b);
  ASSERT_TRUE(pref.has_value());
  EXPECT_EQ(*pref, fx.blackhole_b);
  EXPECT_FALSE(fx.harness->mgr_a().IsConnected(fx.harness->peer_id_b));
}

TEST(AmpPunchCoordinatorTest, Stage6_BurstDialBlackholeAlone) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  // Critical isolation: BurstDial + Pump with no ChannelMux frame on the stack.
  auto pump = [&]() { fx.PumpAll(); };
  auto burst = BurstDialCandidates(fx.harness->mgr_a(), pump, {fx.blackhole_b}, 200);
  EXPECT_FALSE(burst.ok);
  EXPECT_FALSE(burst.error.empty()) << "expected dial/window failure message";
  EXPECT_TRUE(burst.error.find("timed out") != std::string::npos ||
              burst.error.find("window expired") != std::string::npos ||
              burst.error.find("punch burst") != std::string::npos ||
              burst.error.find("dial") != std::string::npos)
      << burst.error;
  EXPECT_FALSE(fx.harness->mgr_a().IsConnected(fx.harness->peer_id_b));
  EXPECT_EQ(fx.harness->mgr_a().CountConnectedLinksForPeerId(fx.harness->peer_id_b), 0u);
}

TEST(AmpPunchCoordinatorTest, Stage7_BurstDialAfterDeferredDrain) {
  PunchExpiryStageFixture fx;
  ASSERT_TRUE(PunchExpiryStageFixture::Create(&fx));
  ASSERT_TRUE(fx.AssocAAndBToIntroducer());

  // Mirrors PostStrand: queue burst work, then drain via pump — never under mux.
  std::vector<std::function<void()>> deferred;
  PunchBurstResult burst;
  deferred.push_back([&] {
    burst = BurstDialCandidates(fx.harness->mgr_a(), [&] { fx.PumpAll(); }, {fx.blackhole_b}, 200);
  });
  fx.PumpAll();
  ASSERT_FALSE(deferred.empty());
  auto work = std::move(deferred.back());
  deferred.pop_back();
  work();
  EXPECT_FALSE(burst.ok);
  EXPECT_FALSE(burst.error.empty()) << burst.error;
  EXPECT_FALSE(fx.harness->mgr_a().IsConnected(fx.harness->peer_id_b));
}

/**
 * L3.25 gap: sync-window expiry — unreachable candidate addrs so burst never auths
 * within the epoch → coded PunchFailed (caller falls through to circuit under H002).
 */
TEST(AmpPunchCoordinatorTest, SyncWindowExpiryReturnsPunchFailed) {
  auto created = pbr::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  // Accept stays on for A↔I / B↔I; punch candidates intentionally blackhole.
  harness->ep_a->SetAcceptEnabled(true);
  harness->ep_b->SetAcceptEnabled(true);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("introducer", harness->ma_r)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_a, harness->ma_a)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_r().RegisterEndpoint(harness->peer_id_b, harness->ma_b)));

  const std::string blackhole_a =
      "/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/" + harness->peer_id_a;
  const std::string blackhole_b =
      "/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/" + harness->peer_id_b;

  auto pump = [&]() { harness->PumpAll(); };
  AmpPunchCoordinator punch_a(harness->mgr_a(), pump, {}, PostIo(*harness->runtime_a));
  AmpPunchCoordinator punch_i(harness->mgr_r(), pump, {}, PostIo(*harness->runtime_r));
  AmpPunchCoordinator punch_b(harness->mgr_b(), pump, {}, PostIo(*harness->runtime_b));
  punch_a.SetLocalCandidateAddrs({blackhole_a});
  punch_i.SetLocalCandidateAddrs({harness->ma_r});
  punch_b.SetLocalCandidateAddrs({blackhole_b});
  punch_a.Start();
  punch_i.Start();
  punch_b.Start();

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
  ASSERT_FALSE(harness->mgr_a().IsConnected(harness->peer_id_b));

  // Short epoch: burst dials blackhole MAs and must not claim a direct PeerLink.
  auto punched = punch_a.TryColdPunch("introducer", harness->peer_id_b, {blackhole_a}, 200);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::PunchFailed);
  const std::string& err = punched.error().message;
  EXPECT_TRUE(err.find("window expired") != std::string::npos ||
              err.find("timed out") != std::string::npos ||
              err.find("punch burst") != std::string::npos)
      << err;
  EXPECT_FALSE(harness->mgr_a().IsConnected(harness->peer_id_b));
  EXPECT_EQ(harness->mgr_a().CountConnectedLinksForPeerId(harness->peer_id_b), 0u);

  punch_a.Stop();
  punch_i.Stop();
  punch_b.Stop();
}

} // namespace

TEST(AmpPunchCoordinatorTest, PublishPunchWinnerAddrsUpsertsPeerIdEndpoint) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  EXPECT_FALSE(harness->mgr_a().GetLinkSnapshot(harness->peer_id_b).has_endpoint);
  PublishPunchWinnerAddrs(harness->mgr_a(), harness->peer_id_b, harness->ma_b);
  EXPECT_TRUE(harness->mgr_a().GetLinkSnapshot(harness->peer_id_b).has_endpoint);
  ASSERT_TRUE(harness->mgr_a().PreferredMultiaddr(harness->peer_id_b).has_value());
  EXPECT_EQ(*harness->mgr_a().PreferredMultiaddr(harness->peer_id_b), harness->ma_b);
}

} // namespace pbr
