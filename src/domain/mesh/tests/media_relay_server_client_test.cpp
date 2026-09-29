#include "domain/mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"
#include "common/directory/RelayScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>

namespace pbr {
namespace {

class MediaRelayServerClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    harness_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("hop", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("client", harness_->ma_a)));

    hop_ = std::make_unique<MediaRelayServer>(*harness_->runtime_b);
    // The hop node's own client (local hop): joins hop_'s sessions without dialing itself.
    hop_client_ = std::make_unique<MediaRelayClientCoordinator>(*harness_->runtime_b, hop_.get());
    client_ = std::make_unique<MediaRelayClientCoordinator>(*harness_->runtime_a);
    hop_->Start();
    hop_client_->Start();
    client_->Start();
  }

  void TearDown() override {
    if (client_) {
      client_->Stop();
    }
    if (hop_client_) {
      hop_client_->Stop();
    }
    if (hop_) {
      hop_->Stop();
    }
    client_.reset();
    hop_client_.reset();
    hop_.reset();
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

    void PumpUntilDone(pbr::test::AmpMeshHarness& harness, const size_t max_rounds = 800) {
      harness.PumpUntil([this] { return done.load(std::memory_order_acquire); }, max_rounds);
      ASSERT_TRUE(done.load(std::memory_order_acquire)) << "media-relay completion timed out";
    }
  };

  std::unique_ptr<pbr::test::AmpMeshHarness> harness_;
  std::unique_ptr<MediaRelayServer> hop_;
  std::unique_ptr<MediaRelayClientCoordinator> hop_client_;
  std::unique_ptr<MediaRelayClientCoordinator> client_;
};

TEST_F(MediaRelayServerClientTest, QuoteRoundTrip) {
  MediaRelayQuoteRequest req;
  req.session_id = "call-amp-quote";
  req.participants = 2;

  Wait<MediaRelayQuote> wait;
  auto id = client_->StartQuote("hop", req, wait.Fn(), 8000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(wait.result) << wait.result.error().message;
  EXPECT_TRUE(wait.result->ok);
  EXPECT_FALSE(wait.result->quote_id.empty());
  EXPECT_EQ(wait.result->pricing_mode, "volunteer");
}

TEST_F(MediaRelayServerClientTest, AcceptAndAttachRoundTrip) {
  MediaRelayQuoteRequest req;
  req.session_id = "call-amp-attach";

  Wait<MediaRelayQuote> quote_wait;
  auto qid = client_->StartQuote("hop", req, quote_wait.Fn(), 8000);
  ASSERT_TRUE(qid);
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result) << quote_wait.result.error().message;
  ASSERT_TRUE(quote_wait.result->ok);

  Wait<MediaRelayAttachResult> attach_wait;
  auto aid = client_->StartAttach("hop", quote_wait.result->quote_id, req.session_id, req.session_id, {},
                                  attach_wait.Fn(), 8000);
  ASSERT_TRUE(aid);
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result) << attach_wait.result.error().message;
  EXPECT_TRUE(attach_wait.result->ok);
  EXPECT_FALSE(attach_wait.result->session_token.empty());
  EXPECT_TRUE(client_->IsAttached());
}

TEST_F(MediaRelayServerClientTest, AdmitRefusesStrangerOnQuote) {
  MediaRelayAdmissionPolicy policy;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  policy.contact_peer_ids = {"not-the-client"};
  hop_->SetAdmissionPolicy(std::move(policy));

  MediaRelayQuoteRequest req;
  req.session_id = "call-stranger";
  Wait<MediaRelayQuote> wait;
  auto id = client_->StartQuote("hop", req, wait.Fn(), 5000);
  ASSERT_TRUE(id);
  wait.PumpUntilDone(*harness_);
  ASSERT_FALSE(wait.result);
  EXPECT_NE(wait.result.error().message.find("stranger"), std::string::npos);
}

TEST_F(MediaRelayServerClientTest, LocalHopFanoutRoundTrip) {
  // Ownership canary (A027): adopt into client_ then EnqueueOutbound (Subscribe) must work.
  const std::string call_id = "call-amp-fanout";
  MediaRelayQuoteRequest req;
  req.session_id = call_id;
  req.participants = 2;

  Wait<MediaRelayQuote> quote_wait;
  ASSERT_TRUE(client_->StartQuote("hop", req, quote_wait.Fn(), 8000));
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result);

  std::atomic<int> guest_frames{0};
  Wait<MediaRelayAttachResult> attach_wait;
  ASSERT_TRUE(client_->StartAttach("hop", quote_wait.result->quote_id, call_id, call_id,
                                   [&guest_frames](MediaDataFrame) { guest_frames.fetch_add(1); },
                                   attach_wait.Fn(), 8000));
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result);

  client_->StartClientFrameReader();

  std::atomic<int> local_frames{0};
  auto local = hop_client_->AttachAsLocalHop(call_id, [&local_frames](MediaDataFrame) { local_frames.fetch_add(1); });
  ASSERT_TRUE(local);

  ASSERT_TRUE(client_->Subscribe(7, 0));
  ASSERT_TRUE(hop_client_->Subscribe(7, 0));
  ASSERT_TRUE(hop_client_->Subscribe(8, 0));
  // Host fan-out applies subscriptions only after the wire JSON is pumped.
  for (int i = 0; i < 40; ++i) {
    harness_->PumpBoth();
  }

  MediaDataFrame uplink;
  uplink.stream_id = 7;
  uplink.channel_id = 0;
  uplink.payload = {1, 2, 3};
  ASSERT_TRUE(hop_client_->SendFrame(uplink));

  harness_->PumpUntil([&] { return guest_frames.load() >= 1; }, 800);
  EXPECT_GE(guest_frames.load(), 1);

  MediaDataFrame downlink;
  downlink.stream_id = 8;
  downlink.channel_id = 0;
  downlink.payload = {4, 5, 6};
  ASSERT_TRUE(client_->SendFrame(downlink));

  harness_->PumpUntil([&] { return local_frames.load() >= 1; }, 800);
  EXPECT_GE(local_frames.load(), 1);

  client_->Detach();
  hop_client_->Detach();
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

class AmpMediaRelayClientLossTest : public MediaRelayServerClientTest {
protected:
  void Attach(const std::string& session) {
    MediaRelayQuoteRequest req;
    req.session_id = session;
    Wait<MediaRelayQuote> quote_wait;
    ASSERT_TRUE(client_->StartQuote("hop", req, quote_wait.Fn(), 8000));
    quote_wait.PumpUntilDone(*harness_);
    ASSERT_TRUE(quote_wait.result && quote_wait.result->ok);
    Wait<MediaRelayAttachResult> attach_wait;
    ASSERT_TRUE(client_->StartAttach("hop", quote_wait.result->quote_id, session, session, {}, attach_wait.Fn(), 8000));
    attach_wait.PumpUntilDone(*harness_);
    ASSERT_TRUE(attach_wait.result && attach_wait.result->ok);
  }
};

// Features register once (calls at mesh wiring) — every later attach must keep the observer, or
// guest reattach-on-loss never runs after the first attach.
TEST_F(AmpMediaRelayClientLossTest, ObserverStaysRegisteredAcrossAttaches) {
  std::atomic<int> lost{0};
  const uint64_t token = client_->AddClientTransportLostObserver([&lost](MediaRelayClientLoss loss) {
    if (loss == MediaRelayClientLoss::TransportLost) {
      lost.fetch_add(1);
    }
  });
  Attach("session-1");
  Attach("session-2");
  hop_->Stop();  // hop goes away: the client channel dies
  harness_->PumpUntil([&lost] { return lost.load() > 0; }, 800);
  EXPECT_EQ(lost.load(), 1);
  client_->RemoveClientTransportLostObserver(token);
}

// Observers hear losses, replacement by another attach and a Detach, each named — so a feature
// that only reattaches on a dead transport never mistakes a replacement for a loss.
TEST_F(AmpMediaRelayClientLossTest, ObserversHearReplacementDetachAndLoss) {
  std::mutex mu;
  std::vector<MediaRelayClientLoss> seen;
  const uint64_t token = client_->AddClientTransportLostObserver([&](MediaRelayClientLoss loss) {
    std::lock_guard lock(mu);
    seen.push_back(loss);
  });
  ASSERT_NE(token, 0u);
  const auto count = [&]() {
    std::lock_guard lock(mu);
    return seen.size();
  };
  Attach("viewer");
  EXPECT_EQ(count(), 0u) << "first attach replaces nothing";
  Attach("call");
  harness_->PumpUntil([&] { return count() >= 1; }, 400);
  client_->Detach();
  harness_->PumpUntil([&] { return count() >= 2; }, 400);
  Attach("again");
  hop_->Stop();
  harness_->PumpUntil([&] { return count() >= 3; }, 800);
  std::lock_guard lock(mu);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], MediaRelayClientLoss::Replaced);
  EXPECT_EQ(seen[1], MediaRelayClientLoss::Detached);
  EXPECT_EQ(seen[2], MediaRelayClientLoss::TransportLost);
  client_->RemoveClientTransportLostObserver(token);
}

// One-way media (a broadcaster only sends, a viewer only receives) leaves one end without RX:
// the client marks its hop link hot so keepalive echoes keep it alive past the 5 s cold window.
TEST_F(AmpMediaRelayClientLossTest, PublishOnlyClientSurvivesLongSilenceFromTheHop) {
  std::atomic<int> lost{0};
  const uint64_t token = client_->AddClientTransportLostObserver([&lost](MediaRelayClientLoss loss) {
    if (loss == MediaRelayClientLoss::TransportLost) {
      lost.fetch_add(1);
    }
  });
  Attach("publish-only");
  for (int i = 0; i < 12000; ++i) {  // 12 s of virtual time, nothing received from the hop
    harness_->PumpBoth();
  }
  EXPECT_EQ(lost.load(), 0);
  EXPECT_TRUE(client_->IsAttached());
  client_->RemoveClientTransportLostObserver(token);
}

// A write the mux refuses (here: an oversized frame) fails the channel inside SendFrame, and its
// closed callback reports the loss. SendFrame used to enqueue under the coordinator lock, which that
// callback takes too: the sending thread (the engine's capture thread) deadlocked, then the pump.
TEST_F(AmpMediaRelayClientLossTest, AWriteThatFailsTheChannelDoesNotDeadlockTheSender) {
  std::atomic<int> lost{0};
  const uint64_t token = client_->AddClientTransportLostObserver([&lost](MediaRelayClientLoss loss) {
    if (loss == MediaRelayClientLoss::TransportLost) {
      lost.fetch_add(1);
    }
  });
  Attach("sender");
  MediaDataFrame oversized;
  oversized.stream_id = 7;
  oversized.payload.assign(32u * 1024u * 1024u, 0xAB);
  auto sent = std::async(std::launch::async, [&]() { return client_->SendFrame(oversized); });
  ASSERT_EQ(sent.wait_for(std::chrono::seconds(5)), std::future_status::ready) << "sender deadlocked";
  (void)sent.get();  // queued, then the mux refused it and failed the channel
  harness_->PumpUntil([&lost] { return lost.load() > 0; }, 200);
  EXPECT_EQ(lost.load(), 1);
  EXPECT_FALSE(client_->IsAttached());
  client_->RemoveClientTransportLostObserver(token);
}

} // namespace
} // namespace pbr
