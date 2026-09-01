#include "base/mesh/CircuitTunnelCoordinator.h"

#include "base/mesh/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "base/mesh/tests/support/mesh_triple_harness.h"
#include "base/people/RelayScope.h"

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
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));

    relay_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    client_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    relay_->Start();
    client_->Start();
  }

  void TearDown() override {
    if (client_) {
      client_->Stop();
    }
    if (relay_) {
      relay_->Stop();
    }
    client_.reset();
    relay_.reset();
    harness_.reset();
  }

  void ArmTargetReader() {
    harness_->mgr_b().SetProtocolHandler(
        kAmpBridgeTargetProtocol, [this](pp::amp::PeerLink& link, const uint32_t channel_id) {
          auto session = std::make_shared<pp::amp::ChannelSession>();
          target_session_ = session;
          session->Bind(*link.Mux(), channel_id, pp::amp::CircuitTunnelChannelPolicy(),
                        [this, session](Roe<std::vector<uint8_t>> frame) {
                          if (!frame) {
                            return false;
                          }
                          std::lock_guard lock(target_mu_);
                          target_received_ = *frame;
                          target_got_ = true;
                          return true;
                        });
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

} // namespace
} // namespace pbr
