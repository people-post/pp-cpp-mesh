#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pbr {
namespace {

/**
 * L3.25c: A already reaches B via circuit R1; ACP upgrade uses R1 as introducer,
 * publishes a direct PeerId endpoint, then demotes the circuit hop.
 */
class AmpPunchCircuitUpgradeTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    harness_->ep_a->SetAcceptEnabled(true);
    harness_->ep_b->SetAcceptEnabled(true);

    // A↔R and R↔B only initially (no direct A↔B book entry).
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_a, harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));

    harness_->mgr_a().EnableNestedCarrierAccept(true);
    harness_->mgr_b().EnableNestedCarrierAccept(true);

    hops_ = std::make_unique<AmpCircuitHopRegistry>();
    circuit_r_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();
    circuit_a_->SetServeInbound(false);

    auto pump = [this]() { harness_->PumpAll(); };
    punch_a_ = std::make_unique<AmpPunchCoordinator>(harness_->mgr_a(), pump);
    punch_r_ = std::make_unique<AmpPunchCoordinator>(harness_->mgr_r(), pump);
    punch_b_ = std::make_unique<AmpPunchCoordinator>(harness_->mgr_b(), pump);
    punch_a_->SetLocalCandidateAddrs({harness_->ma_a});
    punch_r_->SetLocalCandidateAddrs({harness_->ma_r});
    punch_b_->SetLocalCandidateAddrs({harness_->ma_b});
    punch_a_->Start();
    punch_r_->Start();
    punch_b_->Start();
  }

  void TearDown() override {
    if (punch_a_) {
      punch_a_->Stop();
    }
    if (punch_r_) {
      punch_r_->Stop();
    }
    if (punch_b_) {
      punch_b_->Stop();
    }
    if (circuit_a_) {
      circuit_a_->Stop();
    }
    if (circuit_r_) {
      circuit_r_->Stop();
    }
    punch_a_.reset();
    punch_r_.reset();
    punch_b_.reset();
    circuit_a_.reset();
    circuit_r_.reset();
    hops_.reset();
    harness_.reset();
  }

  template <typename Result>
  struct Wait {
    std::atomic<bool> done{false};
    Roe<Result> result = Error("pending");

    std::function<void(Roe<Result>)> Fn() {
      return [this](Roe<Result> r) {
        result = std::move(r);
        done.store(true, std::memory_order_release);
      };
    }

    pp::amp::PeerLinkManager::LinkCb LinkFn() {
      return [this](pp::amp::PeerLinkManager::LinkRoe r) {
        if (r) {
          result = Roe<void>();
        } else {
          result = Error(r.error().message);
        }
        done.store(true, std::memory_order_release);
      };
    }

    void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2500) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  Roe<CircuitTunnelId> EstablishCircuitHop() {
    CircuitBridgeTarget target;
    target.target_peer_id = harness_->peer_id_b;
    target.target_multiaddr = harness_->ma_b;
    target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

    Wait<CircuitTunnelBridgeResult> bridge_wait;
    auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 8000);
    if (!tunnel_id) {
      return Error("start bridge failed");
    }
    bridge_wait.PumpUntilDone(*harness_);
    if (!bridge_wait.result) {
      return Error(bridge_wait.result.error().message);
    }
    if (!bridge_wait.result->ok || !bridge_wait.result->session) {
      return Error(bridge_wait.result->error.empty() ? "bridge refused" : bridge_wait.result->error);
    }

    Wait<void> nested_wait;
    harness_->mgr_a().EstablishNestedOverCarrier(harness_->peer_id_b, bridge_wait.result->session, true,
                                                 nested_wait.LinkFn());
    nested_wait.PumpUntilDone(*harness_);
    if (!nested_wait.result) {
      return Error(nested_wait.result.error().message);
    }
    (void)hops_->Install(harness_->peer_id_b, "relay", pp::amp::kAmpCircuitCarrierProtocolId,
                         bridge_wait.result->session, tunnel_id);
    return Roe<CircuitTunnelId>{tunnel_id};
  }

  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<AmpCircuitHopRegistry> hops_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_a_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_r_;
  std::unique_ptr<AmpPunchCoordinator> punch_a_;
  std::unique_ptr<AmpPunchCoordinator> punch_r_;
  std::unique_ptr<AmpPunchCoordinator> punch_b_;
};

TEST_F(AmpPunchCircuitUpgradeTest, UpgradeViaRelayIntroducerThenDemoteCircuit) {
  bool a_ready = false;
  bool b_ready = false;
  harness_->mgr_a().EnsureAssociation("relay", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    a_ready = static_cast<bool>(r);
  });
  harness_->mgr_b().EnsureAssociation("relay", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    b_ready = static_cast<bool>(r);
  });
  harness_->PumpUntil([&] { return a_ready && b_ready; }, 2000);
  ASSERT_TRUE(a_ready);
  ASSERT_TRUE(b_ready);

  auto tunnel = EstablishCircuitHop();
  ASSERT_TRUE(static_cast<bool>(tunnel)) << tunnel.error().message;
  ASSERT_TRUE(hops_->Find(harness_->peer_id_b, pp::amp::kAmpCircuitCarrierProtocolId).has_value());
  ASSERT_TRUE(harness_->mgr_a().IsConnected(harness_->peer_id_b));

  auto* nested = harness_->mgr_a().FindLinkByPeerId(harness_->peer_id_b);
  ASSERT_NE(nested, nullptr);
  ASSERT_TRUE(nested->IsCarrierBacked());

  auto punched = punch_a_->TryUpgradePunch("relay", harness_->peer_id_b, {harness_->ma_a}, 3000);
  ASSERT_TRUE(static_cast<bool>(punched)) << punched.error().message;
  EXPECT_TRUE(punched->ok) << punched->error;
  EXPECT_FALSE(punched->winner_multiaddr.empty());

  harness_->PumpUntil(
      [&] {
        auto* link = harness_->mgr_a().FindLinkByPeerId(harness_->peer_id_b);
        return link && link->Phase() == pp::amp::PeerLinkPhase::Connected && !link->IsCarrierBacked();
      },
      5000);

  auto* direct = harness_->mgr_a().FindLinkByPeerId(harness_->peer_id_b);
  ASSERT_NE(direct, nullptr);
  EXPECT_FALSE(direct->IsCarrierBacked());
  EXPECT_TRUE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint);

  circuit_a_->CancelTunnel(*tunnel);
  hops_->Clear(harness_->peer_id_b, pp::amp::kAmpCircuitCarrierProtocolId);
  EXPECT_FALSE(hops_->Find(harness_->peer_id_b, pp::amp::kAmpCircuitCarrierProtocolId).has_value());

  direct = harness_->mgr_a().FindLinkByPeerId(harness_->peer_id_b);
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(direct->Phase(), pp::amp::PeerLinkPhase::Connected);
  EXPECT_FALSE(direct->IsCarrierBacked());
}

TEST_F(AmpPunchCircuitUpgradeTest, UpgradePunchUsesUpgradeReason) {
  PunchConnectRequest req;
  req.target_peer_id = "12D3KooWTarget";
  req.addrs = {"/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/12D3KooWSelf"};
  req.window_ms = 1500;
  req.reason = "upgrade";
  const std::string json = EncodePunchConnect(req);
  auto root = TryParseObject(json);
  ASSERT_TRUE(root.has_value());
  auto decoded = DecodePunchConnect(*root);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->reason, "upgrade");
}

} // namespace
} // namespace pbr
