#include "base/p2p/AmpCircuitRelayService.h"

#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/channel/ChannelSession.h"
#include "base/mesh/link/tests/mesh_triple_harness.h"
#include "base/people/RelayScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {
namespace {

inline constexpr const char* kAmpBridgeTargetProtocol = "/pp-browser/circuit-relay-bridge-test/1.0.0";

class AmpCircuitRelayServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    // Optional reverse book entries for peer-id resolution.
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));

    auto pump = [this] {
      if (harness_) {
        harness_->PumpAll();
      }
    };
    relay_service_ = std::make_unique<AmpCircuitRelayService>(*harness_->runtime_r, pump);
    client_service_ = std::make_unique<AmpCircuitRelayService>(*harness_->runtime_a, pump);
    relay_service_->Start();
    client_service_->Start();
  }

  void TearDown() override {
    if (client_service_) {
      client_service_->Stop();
    }
    if (relay_service_) {
      relay_service_->Stop();
    }
    client_service_.reset();
    relay_service_.reset();
    harness_.reset();
  }

  void ArmTargetReader() {
    harness_->mgr_b().SetProtocolHandler(
        kAmpBridgeTargetProtocol, [this](amp::PeerLink& link, const uint32_t channel_id) {
          auto session = std::make_shared<amp::ChannelSession>();
          target_session_ = session;
          session->Bind(*link.Mux(), channel_id, amp::CircuitTunnelChannelPolicy(),
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

  std::unique_ptr<test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<AmpCircuitRelayService> relay_service_;
  std::unique_ptr<AmpCircuitRelayService> client_service_;
  std::shared_ptr<amp::ChannelSession> target_session_;
  std::mutex target_mu_;
  bool target_got_ = false;
  std::vector<uint8_t> target_received_;
};

TEST_F(AmpCircuitRelayServiceTest, BridgeForwardsPayload) {
  ArmTargetReader();

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  auto bridged = client_service_->RequestBridge("relay", target, {}, {}, 8000);
  ASSERT_TRUE(bridged) << bridged.error().message;
  ASSERT_TRUE(bridged->ok) << bridged->error;
  ASSERT_TRUE(bridged->session);

  const std::vector<uint8_t> payload = {'c', 'i', 'r', 'c', 'u', 'i', 't'};
  ASSERT_TRUE(bridged->session->EnqueueOutbound(payload));

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

TEST_F(AmpCircuitRelayServiceTest, BridgeForwardsReversePayload) {
  ArmTargetReader();

  std::mutex client_mu;
  bool client_got = false;
  std::vector<uint8_t> client_received;

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  auto bridged = client_service_->RequestBridge(
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
      {}, 8000);
  ASSERT_TRUE(bridged) << bridged.error().message;
  ASSERT_TRUE(bridged->ok) << bridged->error;
  ASSERT_TRUE(bridged->session);

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

TEST_F(AmpCircuitRelayServiceTest, StrangerRefusedWhenContactsOnly) {
  CircuitRelayAdmissionPolicy policy;
  policy.prefer_contacts_only = true;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  policy.contact_peer_ids = {"not-the-client"};
  relay_service_->SetAdmissionPolicy(std::move(policy));

  CircuitBridgeTarget target;
  target.target_multiaddr = harness_->ma_b;
  target.target_peer_id = harness_->peer_id_b;
  target.target_protocol = kAmpBridgeTargetProtocol;

  auto bridged = client_service_->RequestBridge("relay", target, {}, {}, 5000);
  ASSERT_FALSE(bridged);
  EXPECT_NE(bridged.error().message.find("stranger"), std::string::npos);
}

} // namespace
} // namespace pbr
