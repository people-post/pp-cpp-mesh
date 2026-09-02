#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/media_relay/AmpMediaRelayCoordinator.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"

#include "domain/mesh/tests/support/mesh_triple_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace pbr {
namespace {

/**
 * A has no ADP path to B; SoftMigrate-style quote/attach uses Amp circuit via R.
 */
class AmpCircuitMediaRelayComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);

    // A↔R and R↔B only — A must not dial B directly.
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("a", harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));

    hops_ = std::make_unique<AmpCircuitHopRegistry>();
    circuit_r_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<CircuitTunnelCoordinator>(*harness_->runtime_a);
    hop_ = std::make_unique<AmpMediaRelayCoordinator>(*harness_->runtime_b);
    client_ = std::make_unique<AmpMediaRelayCoordinator>(*harness_->runtime_a);
    client_->SetCircuitHopRegistry(hops_.get());

    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();
    circuit_a_->SetServeInbound(false);
    hop_->Start();
    hop_->SetServeInbound(true);
    client_->Start();
    client_->SetServeInbound(false);
  }

  void TearDown() override {
    if (client_) {
      client_->Stop();
    }
    if (hop_) {
      hop_->Stop();
    }
    if (circuit_a_) {
      circuit_a_->Stop();
    }
    if (circuit_r_) {
      circuit_r_->Stop();
    }
    client_.reset();
    hop_.reset();
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

    void PumpUntilDone(pbr::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 1200) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  std::unique_ptr<pbr::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<AmpCircuitHopRegistry> hops_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_r_;
  std::unique_ptr<CircuitTunnelCoordinator> circuit_a_;
  std::unique_ptr<AmpMediaRelayCoordinator> hop_;
  std::unique_ptr<AmpMediaRelayCoordinator> client_;
};

TEST_F(AmpCircuitMediaRelayComposeTest, CircuitBackedQuoteAndAttach) {
  ASSERT_FALSE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint);

  CircuitBridgeTarget target;
  target.target_peer_id = harness_->peer_id_b;
  target.target_multiaddr = harness_->ma_b;
  target.target_protocol = kMediaRelayProtocolId;

  Wait<CircuitTunnelBridgeResult> bridge_wait;
  auto tunnel_id = circuit_a_->StartBridge("relay", target, {}, {}, bridge_wait.Fn(), 8000);
  ASSERT_TRUE(tunnel_id);
  bridge_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(bridge_wait.result) << bridge_wait.result.error().message;
  ASSERT_TRUE(bridge_wait.result->ok) << bridge_wait.result->error;
  ASSERT_TRUE(bridge_wait.result->session);
  ASSERT_TRUE(static_cast<bool>(
      hops_->Install(harness_->peer_id_b, "relay", kMediaRelayProtocolId, bridge_wait.result->session,
                     tunnel_id)));

  MediaRelayQuoteRequest req;
  req.call_id = "call-amp-circuit-sfu";
  req.participants = 2;

  Wait<MediaRelayQuote> quote_wait;
  ASSERT_TRUE(client_->StartQuote(harness_->peer_id_b, req, quote_wait.Fn(), 8000));
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result) << quote_wait.result.error().message;
  ASSERT_TRUE(quote_wait.result->ok);

  Wait<MediaRelayAttachResult> attach_wait;
  ASSERT_TRUE(client_->StartAttach(harness_->peer_id_b, quote_wait.result->quote_id, req.call_id,
                                   req.call_id, {}, attach_wait.Fn(), 8000));
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result) << attach_wait.result.error().message;
  EXPECT_TRUE(attach_wait.result->ok);
  EXPECT_TRUE(client_->IsAttached());
  EXPECT_TRUE(hops_->Find(harness_->peer_id_b, kMediaRelayProtocolId).has_value());
}

} // namespace
} // namespace pbr
