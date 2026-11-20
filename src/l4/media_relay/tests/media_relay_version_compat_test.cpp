// Release compatibility (pp-browser projects/release-compatibility V4) for /pp-mesh/datagram-relay/1.0.0 control: a
// v9.0.0 message (`v`:1) is served exactly as before; a newer `v` is refused `bad_version`, naming the versions served
// (before attach: refused and closed; attached: answered, the participant kept); the client reads that as
// VersionMismatch.

#include "mesh/l4/media_relay/MediaRelayErr.h"
#include "mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "mesh/l4/media_relay/serve/MediaRelayServer.h"

#include "amp/L3/ChannelSession.h"
#include "common/ValueJson.h"
#include "mesh/l4/shared/L4ProtocolIds.h"
#include "mesh/l4/shared/ProductChannelPolicies.h"
#include "mesh/l4/shared/WireVersion.h"
#include "mesh/testing/mesh_test_harness.h"
#include "mesh/testing/raw_json_exchange.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

class MediaRelayVersionCompatTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    h_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(h_->mgr_a().RegisterEndpoint("hop", h_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(h_->mgr_b().RegisterEndpoint("client", h_->ma_a)));
  }

  test::RawJsonReplies Exchange(const std::vector<std::string>& requests, const size_t want = 1) {
    return test::RawJsonExchange(
        h_->mgr_a(), [this] { h_->PumpBoth(); }, "hop", kMediaRelayProtocolId, requests, want, 2000,
        pp::amp::MediaRelayClientChannelPolicy());
  }

  static common::Object Parse(const std::string& json) {
    auto root = common::TryParseObject(json);
    EXPECT_TRUE(root.has_value()) << json;
    return root ? *root : common::Object{};
  }

  std::unique_ptr<test::AmpMeshHarness> h_;
};

TEST_F(MediaRelayVersionCompatTest, AV9QuoteIsStillServedAndANewerOneIsRefused) {
  MediaRelayServer hop(*h_->runtime_b);
  hop.Start();

  for (const char* version : {R"("v":1,)", ""}) {
    const test::RawJsonReplies replies =
        Exchange({std::string("{") + version + R"("op":"quote","call_id":"call-v1","participants":2})"});
    ASSERT_TRUE(replies.opened);
    ASSERT_EQ(replies.frames.size(), 1u) << version;
    const common::Object quote = Parse(replies.frames.front());
    EXPECT_TRUE(quote.getIf<bool>("ok").value_or(false)) << replies.frames.front();
    EXPECT_FALSE(quote.getString("quote_id").value_or("").empty());
  }

  const test::RawJsonReplies newer = Exchange({R"({"v":2,"op":"quote","call_id":"call-v2","participants":2})"});
  ASSERT_EQ(newer.frames.size(), 1u);
  const common::Object refusal = Parse(newer.frames.front());
  EXPECT_FALSE(refusal.getIf<bool>("ok").value_or(true));
  EXPECT_EQ(refusal.getString("code").value_or(""), "bad_version") << newer.frames.front();
  EXPECT_EQ(SupportedVersionsText(refusal), "1");
  hop.Stop();
}

// A client meeting a hop that refuses its version: VersionMismatch (update needed), not a plain refusal.
TEST_F(MediaRelayVersionCompatTest, AHopsBadVersionIsAVersionMismatch) {
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> sessions;
  h_->runtime_b->SetProtocolHandler(
      kMediaRelayProtocolId, [&](pp::amp::LinkHandle, const std::string& remote_peer_id, const uint32_t channel_id) {
        auto holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
        *holder = h_->mgr_b().BindChannel(
            remote_peer_id, channel_id, pp::amp::MediaRelayClientChannelPolicy(),
            [holder](Roe<std::vector<uint8_t>> frame) {
              if (frame && *holder) {
                const std::string answer =
                    R"({"v":1,"ok":false,"code":"bad_version","supported_versions":[1],"error":"unsupported"})";
                (void)(*holder)->EnqueueOutbound(std::vector<uint8_t>(answer.begin(), answer.end()));
              }
              return true;
            });
        sessions.push_back(*holder);
      });
  MediaRelayClientCoordinator client(*h_->runtime_a);
  client.Start();
  MediaRelayQuoteRequest req;
  req.session_id = "call-refused";
  req.participants = 2;
  std::atomic<bool> done{false};
  MediaRelayRoe<MediaRelayQuote> result = MediaRelayFailure::Of(MediaRelayErr::Generic, "pending");
  ASSERT_TRUE(client.StartQuote(
      "hop", req,
      [&](MediaRelayRoe<MediaRelayQuote> r) {
        result = std::move(r);
        done = true;
      },
      8000));
  h_->PumpUntil([&] { return done.load(); }, 2000);
  ASSERT_TRUE(done.load());
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().GetCode(), MediaRelayErr::VersionMismatch) << result.error().message;
  client.Stop();
  h_->mgr_b().RemoveProtocolHandler(kMediaRelayProtocolId);
}

TEST(MediaRelayVersionCodesTest, BadVersionReadsAsVersionMismatch) {
  EXPECT_EQ(MediaRelayErrFromCode("bad_version"), MediaRelayErr::VersionMismatch);
  EXPECT_EQ(MediaRelayErrFromCode("bad_request"), MediaRelayErr::HopRefused);
  EXPECT_EQ(MediaRelayErrFromCode("from_the_future"), MediaRelayErr::Generic);
  EXPECT_STREQ(MediaRelayRefusalWire(MediaRelayRefusal::BadVersion), "bad_version");
}

} // namespace
} // namespace pp::mesh
