#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"

#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"
#include "common/directory/RelayScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {
namespace {

inline constexpr const char* kAmpBridgeTargetProtocol = "/pp-browser/circuit-relay-bridge-test/1.0.0";

class CircuitTunnelCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));

    relay_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    client_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    client_b_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_b);
    relay_->Start();
    client_->Start();
    client_b_->Start();
  }

  void TearDown() override {
    if (client_b_) {
      client_b_->Stop();
    }
    if (client_) {
      client_->Stop();
    }
    if (relay_) {
      relay_->Stop();
    }
    client_b_.reset();
    client_.reset();
    relay_.reset();
    harness_.reset();
  }

  void ArmTargetReader() {
    harness_->mgr_b().SetProtocolHandler(
        kAmpBridgeTargetProtocol,
        [this](pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id, const uint32_t channel_id) {
          auto session = harness_->mgr_b().BindChannel(
              remote_peer_id, channel_id, pp::amp::CircuitTunnelChannelPolicy(),
              [this](Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return false;
                }
                std::lock_guard lock(target_mu_);
                target_received_ = *frame;
                target_got_ = true;
                return true;
              });
          target_session_ = session;
        });
  }

  struct BridgeWait {
    std::atomic<bool> done{false};
    Roe<CircuitTunnelBridgeResult> result = Error("pending");

    CircuitTunnelCoordinator::BridgeFinished Fn() {
      return [this](Roe<CircuitTunnelBridgeResult> r) {
        result = std::move(r);
        done.store(true, std::memory_order_release);
      };
    }

    void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 800) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire)) << "bridge completion timed out";
    }
  };

  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<CircuitTunnelCoordinator> relay_;
  std::unique_ptr<CircuitTunnelCoordinator> client_;
  std::unique_ptr<CircuitTunnelCoordinator> client_b_;
  std::shared_ptr<pp::amp::ChannelSession> target_session_;
  std::mutex target_mu_;
  bool target_got_ = false;
  std::vector<uint8_t> target_received_;
};

TEST_F(CircuitTunnelCoordinatorTest, BridgeForwardsPayload) {
  ArmTargetReader();

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  BridgeWait wait;
  auto id = client_->StartBridge("relay", target, {}, {}, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  ASSERT_TRUE(wait.result->ok) << wait.result->error;
  ASSERT_TRUE(wait.result->session);

  const std::vector<uint8_t> payload = {'c', 'i', 'r', 'c', 'u', 'i', 't'};
  ASSERT_TRUE(wait.result->session->EnqueueOutbound(payload));
  harness_->PumpUntil([this] {
    std::lock_guard lock(target_mu_);
    return target_got_;
  });
  {
    std::lock_guard lock(target_mu_);
    ASSERT_TRUE(target_got_);
    EXPECT_EQ(target_received_, payload);
  }
}

TEST_F(CircuitTunnelCoordinatorTest, BridgeForwardsReversePayload) {
  ArmTargetReader();

  std::mutex client_mu;
  bool client_got = false;
  std::vector<uint8_t> client_received;

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  BridgeWait wait;
  auto id = client_->StartBridge(
      "relay", target,
      [&](Roe<std::vector<uint8_t>> frame) {
        if (!frame) {
          return false;
        }
        std::lock_guard lock(client_mu);
        client_received = *frame;
        client_got = true;
        return true;
      },
      {}, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  ASSERT_TRUE(wait.result->ok);

  harness_->PumpUntil([this] { return static_cast<bool>(target_session_); });
  ASSERT_TRUE(target_session_);

  const std::vector<uint8_t> payload = {'b', 'a', 'c', 'k'};
  ASSERT_TRUE(target_session_->EnqueueOutbound(payload));
  harness_->PumpUntil([&] {
    std::lock_guard lock(client_mu);
    return client_got;
  });
  {
    std::lock_guard lock(client_mu);
    ASSERT_TRUE(client_got);
    EXPECT_EQ(client_received, payload);
  }
}

TEST_F(CircuitTunnelCoordinatorTest, StrangerRefusedWhenContactsOnly) {
  CircuitRelayAdmissionPolicy policy;
  policy.prefer_contacts_only = true;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  policy.contact_peer_ids = {"not-the-client"};
  relay_->SetAdmissionPolicy(std::move(policy));

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  BridgeWait wait;
  auto id = client_->StartBridge("relay", target, {}, {}, wait.Fn(), 5000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_FALSE(wait.result);
  EXPECT_NE(wait.result.error().message.find("stranger"), std::string::npos);
}

TEST_F(CircuitTunnelCoordinatorTest, ReserveThenBridge) {
  ArmTargetReader();

  // B parks on R first (double-NAT answerer pattern).
  {
    BridgeWait reserve_wait;
    auto rid = client_b_->StartReserve("relay", reserve_wait.Fn(), 15000);
    ASSERT_TRUE(rid);
    reserve_wait.PumpUntilDone(*harness_);
    ASSERT_TRUE(reserve_wait.result) << reserve_wait.result.error().message;
    ASSERT_TRUE(reserve_wait.result->ok) << reserve_wait.result->error;
    EXPECT_EQ(client_b_->Phase(rid), CircuitTunnelPhase::Reserved);
  }

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  BridgeWait wait;
  auto id = client_->StartBridge("relay", target, {}, {}, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  ASSERT_TRUE(wait.result->ok) << wait.result->error;

  const std::vector<uint8_t> payload = {'r', 'e', 's', 'v'};
  ASSERT_TRUE(wait.result->session->EnqueueOutbound(payload));
  harness_->PumpUntil([this] {
    std::lock_guard lock(target_mu_);
    return target_got_;
  });
  {
    std::lock_guard lock(target_mu_);
    ASSERT_TRUE(target_got_);
    EXPECT_EQ(target_received_, payload);
  }
}

TEST_F(CircuitTunnelCoordinatorTest, ReserveThenBridgePeerIdOnly) {
  // Nested call-media: dialer sends peer-id-only. Relay must accept a Connected/reserved
  // target without a dial-book multiaddr (dogfood 997c1c6f).
  ArmTargetReader();

  {
    BridgeWait reserve_wait;
    auto rid = client_b_->StartReserve("relay", reserve_wait.Fn(), 15000);
    ASSERT_TRUE(rid);
    reserve_wait.PumpUntilDone(*harness_);
    ASSERT_TRUE(reserve_wait.result) << reserve_wait.result.error().message;
    ASSERT_TRUE(reserve_wait.result->ok) << reserve_wait.result->error;
  }
  // Link may be keyed as alias/inbound — IsConnected(peer_id) only checks exact key.
  harness_->PumpUntil(
      [this] { return harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b) > 0; });
  ASSERT_GT(harness_->mgr_r().CountConnectedLinksForPeerId(harness_->peer_id_b), 0u);
  // Drop dial-book entry if any — peer-id-only must not depend on RegisterEndpoint.
  // PeerLinkManager has no Unregister; omit MA in the bridge request instead.

  CircuitBridgeTarget target;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;
  ASSERT_TRUE(target.target_multiaddr.empty());

  BridgeWait wait;
  auto id = client_->StartBridge("relay", target, {}, {}, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  ASSERT_TRUE(wait.result->ok) << wait.result->error
                               << " peer-id-only bridge must succeed when target is reserved/Connected";

  const std::vector<uint8_t> payload = {'p', 'i', 'd'};
  ASSERT_TRUE(wait.result->session->EnqueueOutbound(payload));
  harness_->PumpUntil([this] {
    std::lock_guard lock(target_mu_);
    return target_got_;
  });
  {
    std::lock_guard lock(target_mu_);
    ASSERT_TRUE(target_got_);
    EXPECT_EQ(target_received_, payload);
  }
}

} // namespace
} // namespace pbr
