#include "domain/mesh/l4/media_relay/MediaRelayAttach.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pbr {
namespace {

constexpr const char* kHop = "12D3KooWRelayHop";

class FakeDial final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }
  bool IsDialable(const std::string& peer_key) const override {
    return endpoints.count(peer_key) > 0 || relay_routes.count(peer_key) > 0;
  }
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (auto it = endpoints.find(peer_key); it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  void ClearDialBackoff(const std::string& /*peer_key*/) override { ++clear_backoff; }
  void AbortInflightDial(const std::string& /*peer_key*/) override {}
  void ClearPeerCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_set<std::string> relay_routes;
  int clear_backoff = 0;
};

/** Service reach: registers a protocol-keyed route (dialable) when `lands`. */
class FakeServiceReach final : public ICircuitHopReach {
public:
  explicit FakeServiceReach(FakeDial& dial) : dial_(dial) {}
  Roe<void> TryEnsureHopReachable(const std::string& hop) override {
    ++calls;
    if (lands) {
      dial_.relay_routes.insert(hop);
      return {};
    }
    return Error("circuit hop reach failed");
  }
  Roe<void> TryEnsurePeerReachable(const std::string& /*peer*/) override { return Error("unused"); }

  bool lands = true;
  int calls = 0;

private:
  FakeDial& dial_;
};

class FakeRelay final : public IMediaRelayClient {
public:
  Roe<std::string> LocalPeerIdBase58() const override { return std::string("12D3KooWLocal"); }
  bool IsStarted() const override { return true; }
  Roe<MediaRelayQuote> RequestQuote(const std::string& hop, const MediaRelayQuoteRequest& request,
                                    int /*timeout_ms*/) override {
    ++quotes;
    last_quote_hop = hop;
    last_quote = request;
    MediaRelayQuote q;
    q.ok = quote_ok;
    q.error = quote_ok ? "" : "quote refused";
    q.quote_id = "q-1";
    q.a_up_bps = 32000;
    q.rate = rate;
    return q;
  }
  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& hop, const std::string& quote_id,
                                              const std::string& session_id, const std::string& auth,
                                              std::function<void(MediaDataFrame)> on_frame,
                                              int /*timeout_ms*/) override {
    ++attaches;
    last_attach = {hop, quote_id, session_id, auth};
    frame_sink = std::move(on_frame);
    MediaRelayAttachResult r;
    r.ok = attach_ok;
    r.error = attach_ok ? "" : "attach refused";
    return r;
  }
  void StartClientFrameReader() override {}
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string&, std::function<void(MediaDataFrame)>) override {
    return Error("unused");
  }
  Roe<void> Subscribe(uint32_t, uint16_t) override { return {}; }
  Roe<void> SendFrame(const MediaDataFrame&) override { return {}; }
  void Detach() override {}
  bool IsAttached() const override { return false; }
  bool IsLocalHopAttached() const override { return false; }

  struct AttachArgs {
    std::string hop, quote_id, session_id, auth;
  };
  bool quote_ok = true;
  bool attach_ok = true;
  double rate = 0.0;
  int quotes = 0;
  int attaches = 0;
  std::string last_quote_hop;
  MediaRelayQuoteRequest last_quote;
  AttachArgs last_attach;
  std::function<void(MediaDataFrame)> frame_sink;
};

class MediaRelayAttachTest : public ::testing::Test {
protected:
  MediaRelayAttachPorts Ports() {
    MediaRelayAttachPorts p;
    p.relay = &relay_;
    p.dial = &dial_;
    p.service_reach = &reach_;
    return p;
  }
  static MediaRelayAttachRequest Request(const std::string& multiaddr = {}) {
    MediaRelayAttachRequest r;
    r.hop_peer_id = kHop;
    r.hop_multiaddr = multiaddr;
    r.session_id = "session-1";
    r.auth = "auth-1";
    r.quote.session_id = "session-1";
    r.quote.participants = 3;
    return r;
  }
  std::optional<Roe<MediaRelayAttached>> Run(MediaRelayAttachRequest request, MediaRelayAttachHooks hooks = {}) {
    std::optional<Roe<MediaRelayAttached>> out;
    AttachToMediaRelayAsync(Ports(), std::move(request), std::move(hooks),
                            [&out](Roe<MediaRelayAttached> r) { out = std::move(r); });
    return out;
  }

  FakeDial dial_;
  FakeServiceReach reach_{dial_};
  FakeRelay relay_;
};

TEST_F(MediaRelayAttachTest, DialableHopQuotesThenAttachesWithOpaqueSession) {
  bool framed = false;
  MediaRelayAttachHooks hooks;
  hooks.on_frame = [&framed](MediaDataFrame) { framed = true; };
  auto out = Run(Request("/ip4/203.0.113.9/udp/1/adp/1.0.0/p2p/12D3KooWRelayHop"), std::move(hooks));
  ASSERT_TRUE(out && *out) << (out ? out->error().message : "no completion");
  EXPECT_EQ((*out)->quote_id, "q-1");
  EXPECT_EQ((*out)->a_up_bps, 32000);
  EXPECT_EQ(dial_.clear_backoff, 1) << "hint registered and backoff cleared";
  EXPECT_EQ(reach_.calls, 0) << "no service reach when already dialable";
  EXPECT_EQ(relay_.last_quote.participants, 3) << "caller-built quote passed through";
  EXPECT_EQ(relay_.last_attach.session_id, "session-1");
  EXPECT_EQ(relay_.last_attach.auth, "auth-1");
  ASSERT_TRUE(relay_.frame_sink);
  relay_.frame_sink({});
  EXPECT_TRUE(framed) << "frames go to the caller's sink";
}

TEST_F(MediaRelayAttachTest, UndialableHopUsesServiceReachFirst) {
  auto out = Run(Request());
  ASSERT_TRUE(out && *out);
  EXPECT_EQ(reach_.calls, 1);
  EXPECT_EQ(relay_.attaches, 1);
}

TEST_F(MediaRelayAttachTest, ServiceReachMissFailsBeforeQuote) {
  reach_.lands = false;
  auto out = Run(Request());
  ASSERT_TRUE(out);
  ASSERT_FALSE(*out);
  EXPECT_EQ(out->error().message, "hop not dialable");
  EXPECT_EQ(relay_.quotes, 0);
}

TEST_F(MediaRelayAttachTest, QuoteGateRejectsWithoutAttaching) {
  relay_.rate = 0.25;
  MediaRelayAttachHooks hooks;
  hooks.accept_quote = [](const MediaRelayQuote& q) -> Roe<void> {
    return q.rate > 0 ? Roe<void>(Error("paid hop")) : Roe<void>();
  };
  auto out = Run(Request("/ip4/203.0.113.9/udp/1"), std::move(hooks));
  ASSERT_TRUE(out);
  ASSERT_FALSE(*out);
  EXPECT_EQ(out->error().message, "paid hop");
  EXPECT_EQ(relay_.attaches, 0);
}

TEST_F(MediaRelayAttachTest, NoLongerWantedAbortsAfterQuote) {
  MediaRelayAttachHooks hooks;
  hooks.still_wanted = []() { return false; };
  auto out = Run(Request("/ip4/203.0.113.9/udp/1"), std::move(hooks));
  ASSERT_TRUE(out);
  ASSERT_FALSE(*out);
  EXPECT_EQ(out->error().message, "attach aborted");
  EXPECT_EQ(relay_.quotes, 1);
  EXPECT_EQ(relay_.attaches, 0);
}

TEST_F(MediaRelayAttachTest, RelayRefusalsSurface) {
  relay_.quote_ok = false;
  auto quoted = Run(Request("/ip4/203.0.113.9/udp/1"));
  ASSERT_TRUE(quoted);
  ASSERT_FALSE(*quoted);
  EXPECT_EQ(quoted->error().message, "quote refused");

  relay_.quote_ok = true;
  relay_.attach_ok = false;
  auto attached = Run(Request("/ip4/203.0.113.9/udp/1"));
  ASSERT_TRUE(attached);
  ASSERT_FALSE(*attached);
  EXPECT_EQ(attached->error().message, "attach refused");
}

TEST_F(MediaRelayAttachTest, MissingPortsFail) {
  std::optional<Roe<MediaRelayAttached>> out;
  AttachToMediaRelayAsync(MediaRelayAttachPorts{}, Request(), {}, [&out](Roe<MediaRelayAttached> r) { out = r; });
  ASSERT_TRUE(out);
  EXPECT_FALSE(*out);
}

} // namespace
} // namespace pbr
