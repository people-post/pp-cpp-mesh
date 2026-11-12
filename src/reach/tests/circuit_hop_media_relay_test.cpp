#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/host/MeshPorts.h"
#include "mesh/reach/AmpCircuitHopReach.h"

#include "mesh/testing/mesh_triple_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

/**
 * A hop (B) reachable only through a circuit relay (R): A has no ADP path to B. Reach builds a link to B nested
 * through R (A024, as a 1:1 call's), and the media relay runs on it as on a direct link — both its channels, the
 * control channel and the best-effort data lane.
 */
class AmpCircuitMediaRelayComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pp::mesh::test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);
    harness_->ep_a->SetAcceptEnabled(true);
    harness_->ep_b->SetAcceptEnabled(true);

    // A↔R and R↔B only — A must not dial B directly.
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("relay", harness_->ma_r)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_a, harness_->ma_a)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_r().RegisterEndpoint(harness_->peer_id_b, harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("relay", harness_->ma_r)));
    harness_->mgr_a().EnableNestedCarrierAccept(true);
    harness_->mgr_b().EnableNestedCarrierAccept(true);

    hops_ = std::make_unique<AmpCircuitHopRegistry>();
    links_a_ = NewAmpChatPeerLinks(*harness_->runtime_a);
    circuit_r_ = std::make_unique<CircuitRelayServer>(*harness_->runtime_r);
    circuit_a_ = std::make_unique<CircuitClientCoordinator>(*harness_->runtime_a);
    hop_ = std::make_unique<MediaRelayServer>(*harness_->runtime_b);
    hop_client_ = std::make_unique<MediaRelayClientCoordinator>(*harness_->runtime_b, hop_.get());
    client_ = std::make_unique<MediaRelayClientCoordinator>(*harness_->runtime_a);
    reach_ = std::make_unique<AmpCircuitHopReach>(
        *circuit_a_, *hops_, *links_a_, [this]() { harness_->PumpAll(); },
        [](const std::string&) { return std::vector<std::string>{"relay"}; }, AmpCircuitHopReach::TryPunchAsync{},
        AmpCircuitHopReach::TryPunchViaIntroducerAsync{},
        [rt = harness_->runtime_a.get()](std::function<void()> task) { rt->PostToIo(std::move(task)); },
        [rt = harness_->runtime_a.get()](std::chrono::milliseconds delay, std::function<void()> task) {
          (void)rt->PostAfter(delay, std::move(task));
        });
    reach_->SetClock([clock = harness_->clock]() { return clock->NowMs(); });

    circuit_r_->Start();
    circuit_r_->SetServeInbound(true);
    circuit_a_->Start();
    hop_->Start();
    hop_->SetServeInbound(true);
    hop_client_->Start();
    client_->Start();

    // The hop holds its link to the relay (a node behind NAT, parked there).
    std::atomic<bool> linked{false};
    harness_->mgr_b().EnsureAssociation("relay", [&linked](pp::amp::PeerLinkManager::LinkRoe) { linked = true; });
    harness_->PumpUntil([&linked] { return linked.load(); });
    ASSERT_TRUE(harness_->mgr_b().IsConnected("relay"));
  }

  void TearDown() override {
    if (reach_) {
      reach_->AbortPending();
    }
    for (auto* c : {client_.get(), hop_client_.get()}) {
      if (c) {
        c->Stop();
      }
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
    reach_.reset();
    client_.reset();
    hop_client_.reset();
    hop_.reset();
    circuit_a_.reset();
    circuit_r_.reset();
    links_a_.reset();
    hops_.reset();
    harness_.reset();
  }

  template <typename Result, typename E = Error>
  struct Wait {
    using RoeT = pp::ResultOrError<Result, E>;
    std::atomic<bool> done{false};
    RoeT result = Pending();

    static E Pending() {
      E pending;
      pending.message = "pending";
      return pending;
    }

    std::function<void(RoeT)> Fn() {
      return [this](RoeT r) {
        result = std::move(r);
        done.store(true, std::memory_order_release);
      };
    }

    void PumpUntilDone(pp::mesh::test::AmpMeshTripleHarness& harness, const size_t max_rounds = 2500) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire));
    }
  };

  std::unique_ptr<pp::mesh::test::AmpMeshTripleHarness> harness_;
  std::unique_ptr<AmpCircuitHopRegistry> hops_;
  std::unique_ptr<IChatPeerLinks> links_a_;
  std::unique_ptr<CircuitRelayServer> circuit_r_;
  std::unique_ptr<CircuitClientCoordinator> circuit_a_;
  std::unique_ptr<MediaRelayServer> hop_;
  std::unique_ptr<MediaRelayClientCoordinator> hop_client_;
  std::unique_ptr<MediaRelayClientCoordinator> client_;
  std::unique_ptr<AmpCircuitHopReach> reach_;
};

// V065: media rides a best-effort data lane on every path. A hop behind a circuit used to be reached by a tunnel of
// the media-relay protocol alone: one reliable channel, media on it, no lane.
TEST_F(AmpCircuitMediaRelayComposeTest, AHopBehindACircuitGetsADataLaneAndMediaFlowsBothWays) {
  ASSERT_FALSE(harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b).has_endpoint);

  Wait<void, CircuitReachFailure> reached;
  reach_->TryEnsureHopReachableAsync(harness_->peer_id_b, reached.Fn());
  reached.PumpUntilDone(*harness_);
  ASSERT_TRUE(reached.result) << reached.result.error().message;
  const auto link = harness_->mgr_a().GetLinkSnapshot(harness_->peer_id_b);
  ASSERT_EQ(link.phase, pp::amp::PeerLinkPhase::Connected);
  EXPECT_TRUE(link.carrier_backed) << "a link nested through the relay";

  const std::string call_id = "call-amp-circuit-sfu";
  MediaRelayQuoteRequest req;
  req.session_id = call_id;
  req.participants = 2;
  Wait<MediaRelayQuote, MediaRelayFailure> quote_wait;
  ASSERT_TRUE(client_->StartQuote(harness_->peer_id_b, req, quote_wait.Fn(), 8000));
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result) << quote_wait.result.error().message;
  ASSERT_TRUE(quote_wait.result->ok);

  std::atomic<int> guest_frames{0};
  Wait<MediaRelayAttachResult, MediaRelayFailure> attach_wait;
  ASSERT_TRUE(client_->StartAttach(
      harness_->peer_id_b, quote_wait.result->quote_id, call_id, call_id,
      [&guest_frames](MediaDataFrame) { guest_frames.fetch_add(1); }, attach_wait.Fn(), 8000));
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result) << attach_wait.result.error().message;
  EXPECT_TRUE(attach_wait.result->ok);
  EXPECT_TRUE(client_->IsAttached());
  EXPECT_TRUE(client_->HealthSnapshot().data_lane) << "the attach is done once its lane is open";
  harness_->PumpUntil([&] { return hop_->RuntimeStats().data_lane_participants == 1; });
  EXPECT_EQ(hop_->RuntimeStats().data_lane_participants, 1u);
  client_->StartClientFrameReader();

  std::atomic<int> local_frames{0};
  ASSERT_TRUE(hop_client_->AttachAsLocalHop(call_id, [&local_frames](MediaDataFrame) { local_frames.fetch_add(1); }));
  ASSERT_TRUE(client_->Subscribe(7, 0));
  ASSERT_TRUE(hop_client_->Subscribe(8, 0));
  for (int i = 0; i < 60; ++i) {
    harness_->PumpAll();
  }

  MediaDataFrame from_guest;
  from_guest.stream_id = 8;
  from_guest.payload = {4, 5, 6};
  ASSERT_TRUE(client_->SendFrame(from_guest));
  harness_->PumpUntil([&] { return local_frames.load() >= 1; });
  EXPECT_GE(local_frames.load(), 1) << "up the lane, through the relay";

  MediaDataFrame to_guest;
  to_guest.stream_id = 7;
  to_guest.payload = {1, 2, 3};
  ASSERT_TRUE(hop_client_->SendFrame(to_guest));
  harness_->PumpUntil([&] { return guest_frames.load() >= 1; });
  EXPECT_GE(guest_frames.load(), 1) << "down the lane, through the relay";
  EXPECT_EQ(client_->HealthSnapshot().lane_wait_drops, 0u);

  client_->Detach();
  hop_client_->Detach();
}

} // namespace
} // namespace pp::mesh
