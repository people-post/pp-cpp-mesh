#include "domain/mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"
#include "common/directory/RelayScope.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <optional>
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
  // Call details on a hop: the figures of the association to the relay.
  const CallHopHealth health = client_->HealthSnapshot();
  EXPECT_TRUE(health.attached);
  EXPECT_TRUE(health.link.available);
  EXPECT_GT(health.link.reliable_sent, 0u);
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

// Shutdown order (MeshHost::Stop): L4 servers are freed before Amp tears down their channels. A
// relay channel still mid-handshake then ends after the server is gone, and its closed callback
// must not reach the freed server (heap-use-after-free under ASan before it was bound to the
// server's lifetime token).
TEST_F(MediaRelayServerClientTest, ChannelEndingAfterTheServerIsFreedDoesNotTouchIt) {
  std::optional<uint32_t> channel;
  harness_->mgr_a().OpenChannel("hop", kMediaRelayProtocolId, pp::amp::MediaRelayClientChannelPolicy(),
                                [&](const pp::amp::PeerLinkManager::ChannelRoe& ch) {
                                  if (ch.isOk()) {
                                    channel = ch.value();
                                  }
                                });
  harness_->PumpUntil([&] {
    auto* link = harness_->mgr_a().FindLink("hop");
    return channel && link && link->Mux() && link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
  });
  ASSERT_TRUE(channel.has_value());

  hop_client_->Stop();  // holds the server (local hop)
  hop_client_.reset();
  hop_->Stop();
  hop_.reset();  // the hop's inbound channel is still bound, mid-handshake

  auto* link = harness_->mgr_a().FindLink("hop");
  ASSERT_TRUE(link && link->Mux());
  ASSERT_TRUE(static_cast<bool>(link->Mux()->CloseChannel(*channel)));
  for (int i = 0; i < 50; ++i) {
    harness_->PumpBoth();  // the hop sees the CLOSE: its closed callback runs
  }
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
  // This node's own media, out to the guest and in from it, is not relaying for others.
  EXPECT_EQ(hop_->RuntimeStats().bytes_relayed, 0u);

  client_->Detach();
  hop_client_->Detach();
}

// B009: the relay answers the publisher's offer with the levels it carries, and drops that
// publisher's video of any other level — and any reserved channel — at ingest.
TEST_F(MediaRelayServerClientTest, RelayCarriesOnlyTheVideoLevelsItAnswered) {
  hop_->SetVideoPolicy(MediaRelayVideoPolicy{{2}, /*carry_levels=*/1, /*strict=*/false});
  const std::string session = "show-levels";
  MediaRelayQuoteRequest req;
  req.session_id = session;
  req.video_levels = {1, 2};
  req.video_parallel = 2;

  Wait<MediaRelayQuote> quote_wait;
  ASSERT_TRUE(client_->StartQuote("hop", req, quote_wait.Fn(), 8000));
  quote_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(quote_wait.result && quote_wait.result->ok);
  EXPECT_EQ(quote_wait.result->video_levels, std::vector<uint8_t>{2}) << "the relay's level, one of them";

  Wait<MediaRelayAttachResult> attach_wait;
  ASSERT_TRUE(client_->StartAttach("hop", quote_wait.result->quote_id, session, session, {}, attach_wait.Fn(), 8000));
  attach_wait.PumpUntilDone(*harness_);
  ASSERT_TRUE(attach_wait.result && attach_wait.result->ok);

  constexpr uint16_t kReservedChannel = 0x21;
  std::atomic<int> low{0};
  std::atomic<int> high{0};
  std::atomic<int> reserved{0};
  ASSERT_TRUE(hop_client_->AttachAsLocalHop(session, [&](MediaDataFrame frame) {
    if (frame.channel_id == kReservedChannel) {
      reserved.fetch_add(1);
      return;
    }
    (frame.channel_id == VideoChannel(1) ? low : high).fetch_add(1);
  }));
  ASSERT_TRUE(hop_client_->Subscribe(7, VideoChannel(1)));
  ASSERT_TRUE(hop_client_->Subscribe(7, VideoChannel(2)));
  ASSERT_TRUE(hop_client_->Subscribe(7, kReservedChannel));
  for (int i = 0; i < 40; ++i) {
    harness_->PumpBoth();
  }

  MediaDataFrame frame;
  frame.stream_id = 7;
  frame.channel_type = MediaChannelType::LatestLossy;
  frame.mark = 1;
  frame.payload = {1, 2, 3};
  frame.channel_id = VideoChannel(1);
  ASSERT_TRUE(client_->SendFrame(frame));
  frame.channel_id = kReservedChannel;
  frame.seq = 1;
  ASSERT_TRUE(client_->SendFrame(frame));
  frame.channel_id = VideoChannel(2);
  frame.seq = 2;
  ASSERT_TRUE(client_->SendFrame(frame));
  harness_->PumpUntil([&] { return high.load() >= 1; }, 800);
  for (int i = 0; i < 40; ++i) {
    harness_->PumpBoth();
  }
  EXPECT_EQ(high.load(), 1);
  EXPECT_EQ(low.load(), 0) << "a level the relay did not agree to carry was fanned out";
  EXPECT_EQ(reserved.load(), 0) << "a reserved channel was fanned out";
  client_->Detach();
  hop_client_->Detach();
}

TEST_F(MediaRelayServerClientTest, StrictRelayRefusesAPublisherWithoutItsLevel) {
  hop_->SetVideoPolicy(MediaRelayVideoPolicy{{2}, 1, /*strict=*/true});
  MediaRelayQuoteRequest req;
  req.session_id = "show-strict";
  req.video_levels = {1};
  Wait<MediaRelayQuote> wait;
  ASSERT_TRUE(client_->StartQuote("hop", req, wait.Fn(), 5000));
  wait.PumpUntilDone(*harness_);
  ASSERT_FALSE(wait.result);
  EXPECT_NE(wait.result.error().message.find("video level"), std::string::npos);
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

// A participant whose channel ends is released: a client leaving (Detach closes the channel; no
// detach op is sent) used to stay in the hop's session for the node's lifetime — participants
// piled up with every call, and the pump kept fanning out to them.
TEST_F(AmpMediaRelayClientLossTest, HopReleasesAParticipantWhoseChannelEnds) {
  for (int round = 0; round < 3; ++round) {
    Attach("leaving");
    EXPECT_EQ(hop_->RuntimeStats().active_participants, 1u);
    client_->Detach();
    harness_->PumpUntil([this] { return hop_->RuntimeStats().active_participants == 0; }, 800);
    EXPECT_EQ(hop_->RuntimeStats().active_participants, 0u) << "round " << round;
  }
}

// Call-scoped admission (CALLS.md): a non-contact may join a call while its session exists. A
// session outlives its last participant by a grace — everyone dropping at once (links flap) and
// re-attaching in any order still works — and is dropped after it: the call is over, and a new
// session for the call id needs contact admission again (it used to be hosted forever).
TEST_F(AmpMediaRelayClientLossTest, EmptiedSessionAdmitsNonContactsOnlyWithinItsGrace) {
  hop_->SetEmptySessionGraceForTest(std::chrono::milliseconds(300));
  MediaRelayAdmissionPolicy contacts_only;
  contacts_only.serve_scope_mask = kRelayScopeLinkSiteSocial;
  contacts_only.contact_peer_ids = {harness_->peer_id_a};
  hop_->SetAdmissionPolicy(contacts_only);
  Attach("scoped-call");  // a contact opens the session

  contacts_only.contact_peer_ids = {"someone-else"};  // the client is now a non-contact
  hop_->SetAdmissionPolicy(contacts_only);
  auto quote = [this]() {
    MediaRelayQuoteRequest req;
    req.session_id = "scoped-call";
    Wait<MediaRelayQuote> wait;
    EXPECT_TRUE(client_->StartQuote("hop", req, wait.Fn(), 5000));
    wait.PumpUntilDone(*harness_);
    return std::move(wait.result);
  };
  EXPECT_TRUE(quote()) << "session live: call-scoped admission";

  client_->Detach();
  harness_->PumpUntil([this] { return hop_->RuntimeStats().active_participants == 0; }, 800);
  EXPECT_TRUE(quote()) << "emptied, within the grace: still the call's session";

  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  harness_->PumpBoth();  // the hop's tick drops the expired session
  auto refused = quote();
  ASSERT_FALSE(refused) << "after the grace the call is over";
  EXPECT_NE(refused.error().message.find("stranger"), std::string::npos) << refused.error().message;
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
