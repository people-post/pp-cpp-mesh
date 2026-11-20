// Release compatibility (pp-browser projects/release-compatibility T10, V4, U7) for the reach protocols: a v9.0.0
// message (`v`:1) is served exactly as before; a newer `v` is refused `bad_version`, naming the versions served; an
// unknown or wrong-phase punch op is answered, not ignored. Each server is spoken to by hand, as an older or a newer
// peer would.

#include "mesh/reachability/dial_back/client/DialBackClient.h"
#include "mesh/reachability/dial_back/serve/DialBackServer.h"
#include "mesh/reachability/punch/AmpPunchCoordinator.h"
#include "mesh/reachability/punch/PunchLogic.h"
#include "mesh/reachability/punch/PunchTypes.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "common/ValueJson.h"
#include "mesh/l4/shared/WireVersion.h"
#include "mesh/testing/mesh_test_harness.h"
#include "mesh/testing/raw_json_exchange.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

using test::RawJsonExchange;
using test::RawJsonReplies;

common::Object Parse(const std::string& json) {
  auto root = common::TryParseObject(json);
  EXPECT_TRUE(root.has_value()) << json;
  return root ? *root : common::Object{};
}

/** A (opener) ↔ B (server), associated. */
class ReachVersionCompatTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    h_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(h_->mgr_a().RegisterEndpoint("server", h_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(h_->mgr_b().RegisterEndpoint("client", h_->ma_a)));
  }

  RawJsonReplies Exchange(const char* protocol_id, const std::vector<std::string>& requests, size_t want = 1) {
    return RawJsonExchange(h_->mgr_a(), [this] { h_->PumpBoth(); }, "server", protocol_id, requests, want);
  }

  std::unique_ptr<test::AmpMeshHarness> h_;
};

// ---- punch (T10, V4) ----

class PunchVersionCompatTest : public ReachVersionCompatTest {
protected:
  void SetUp() override {
    ReachVersionCompatTest::SetUp();
    punch_ = std::make_unique<AmpPunchCoordinator>(*h_->runtime_b, [this] { h_->PumpBoth(); });
    punch_->SetLocalCandidateAddrs({h_->ma_b});
    punch_->Start();
  }
  void TearDown() override {
    if (punch_) {
      punch_->Stop();
    }
  }

  std::string Offer(const char* version_field) const {
    return std::string("{") + version_field + R"("op":"offer","initiator_peer_id":")" + h_->peer_id_a +
           R"(","addrs":[],"epoch_id":"ep-1","window_ms":500})";
  }

  std::unique_ptr<AmpPunchCoordinator> punch_;
};

TEST_F(PunchVersionCompatTest, AV9OfferIsStillServed) {
  for (const char* version : {R"("v":1,)", ""}) { // v9.0.0 sends 1; a missing v reads as 1
    const RawJsonReplies replies = Exchange(kAmpPunchProtocolId, {Offer(version)});
    ASSERT_TRUE(replies.opened);
    ASSERT_EQ(replies.frames.size(), 1u) << version;
    const common::Object reply = Parse(replies.frames.front());
    EXPECT_EQ(reply.getString("op").value_or(""), "candidates") << replies.frames.front();
    EXPECT_EQ(reply.getString("peer_id").value_or(""), h_->peer_id_b);
    EXPECT_EQ(reply.getString("nonce").value_or(""), "ep-1");
  }
}

TEST_F(PunchVersionCompatTest, ANewerVersionIsRefusedByCodeNamingOurs) {
  const RawJsonReplies replies = Exchange(kAmpPunchProtocolId, {Offer(R"("v":2,)")});
  ASSERT_EQ(replies.frames.size(), 1u);
  const common::Object reply = Parse(replies.frames.front());
  EXPECT_EQ(reply.getString("op").value_or(""), "result");
  EXPECT_FALSE(reply.getIf<bool>("ok").value_or(true));
  EXPECT_EQ(reply.getString("code").value_or(""), "bad_version") << replies.frames.front();
  EXPECT_EQ(SupportedVersionsText(reply), "1");
  auto decoded = DecodePunchResult(reply);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->refusal, PunchRefusal::BadVersion);
}

TEST_F(PunchVersionCompatTest, AnUnknownOpIsAnsweredNotLeftToTimeOut) {
  const RawJsonReplies replies = Exchange(kAmpPunchProtocolId, {R"({"v":1,"op":"frobnicate"})"});
  ASSERT_EQ(replies.frames.size(), 1u) << "answered within the pump budget, not left to the 8 s deadline";
  const common::Object reply = Parse(replies.frames.front());
  EXPECT_EQ(reply.getString("op").value_or(""), "result");
  EXPECT_EQ(reply.getString("code").value_or(""), "bad_request") << replies.frames.front();
}

TEST_F(PunchVersionCompatTest, AWrongPhaseOpIsAnswered) {
  // A sync before any offer: this server waits for a first message, and a sync is not one.
  const RawJsonReplies replies =
      Exchange(kAmpPunchProtocolId, {R"({"v":1,"op":"sync","epoch_id":"ep-1","peer_addrs":[]})"});
  ASSERT_EQ(replies.frames.size(), 1u);
  EXPECT_EQ(Parse(replies.frames.front()).getString("code").value_or(""), "bad_request") << replies.frames.front();

  // An offer, then another offer where a sync is due: candidates, then a refusal.
  const RawJsonReplies twice = Exchange(kAmpPunchProtocolId, {Offer(R"("v":1,)"), Offer(R"("v":1,)")}, 2);
  ASSERT_EQ(twice.frames.size(), 2u);
  EXPECT_EQ(Parse(twice.frames[0]).getString("op").value_or(""), "candidates");
  EXPECT_EQ(Parse(twice.frames[1]).getString("code").value_or(""), "bad_request") << twice.frames[1];
}

TEST_F(PunchVersionCompatTest, NotJsonIsAnswered) {
  const RawJsonReplies replies = Exchange(kAmpPunchProtocolId, {"not json"});
  ASSERT_EQ(replies.frames.size(), 1u);
  EXPECT_EQ(Parse(replies.frames.front()).getString("code").value_or(""), "bad_request");
}

// The initiator side: an introducer's bad_version is VersionMismatch (one that may be passed over), not a protocol
// error; a v9.0.0 introducer's refusal without a code stays what it was.
TEST_F(ReachVersionCompatTest, AnIntroducersBadVersionIsAVersionMismatch) {
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> sessions;
  std::string answer;
  h_->runtime_b->SetProtocolHandler(
      kAmpPunchProtocolId, [&](pp::amp::LinkHandle, const std::string& remote_peer_id, const uint32_t channel_id) {
        auto policy = pp::amp::ControlJsonChannelPolicy();
        policy.read_once = false;
        auto holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
        *holder = h_->mgr_b().BindChannel(
            remote_peer_id, channel_id, policy, [holder, &answer](Roe<std::vector<uint8_t>> frame) {
              if (frame && *holder) {
                (void)(*holder)->EnqueueOutbound(std::vector<uint8_t>(answer.begin(), answer.end()));
              }
              return false;
            });
        sessions.push_back(*holder);
      });
  AmpPunchCoordinator initiator(*h_->runtime_a, [this] { h_->PumpBoth(); });
  initiator.Start();

  answer = R"({"v":1,"op":"result","epoch_id":"","ok":false,"code":"bad_version","supported_versions":[1],)"
           R"("error":"punch: unsupported version 2"})";
  auto refused = initiator.TryColdPunch("server", "QmTarget", {h_->ma_a}, 500);
  ASSERT_FALSE(static_cast<bool>(refused));
  EXPECT_EQ(refused.error().GetCode(), PunchErr::VersionMismatch) << refused.error().message;

  answer = R"({"v":1,"op":"result","epoch_id":"","ok":false,"error":"something"})"; // a v9.0.0 refusal: no code
  auto plain = initiator.TryColdPunch("server", "QmTarget", {h_->ma_a}, 500);
  ASSERT_FALSE(static_cast<bool>(plain));
  EXPECT_EQ(plain.error().GetCode(), PunchErr::ProtocolError);

  initiator.Stop();
  h_->mgr_b().RemoveProtocolHandler(kAmpPunchProtocolId);
}

TEST(PunchWireTest, BadVersionRoundTripsByCode) {
  PunchResult result;
  result.ok = false;
  result.refusal = PunchRefusal::BadVersion;
  result.error = "x";
  const common::Object wire = Parse(EncodePunchResult(result));
  EXPECT_EQ(wire.getString("code").value_or(""), "bad_version");
  EXPECT_EQ(SupportedVersionsText(wire), "1");
  EXPECT_EQ(PunchRefusalFromResult("bad_version"), PunchRefusal::BadVersion);
  EXPECT_EQ(PunchRefusalFromResult("from_the_future"), PunchRefusal::Generic);
  EXPECT_EQ(wire.getIf<int64_t>("v").value_or(0), kPunchWireVersion);
}

// ---- dial-back (V4: `v` first, and a `code`) ----

class DialBackVersionCompatTest : public ReachVersionCompatTest {
protected:
  void SetUp() override {
    ReachVersionCompatTest::SetUp();
    seed_ = std::make_unique<DialBackServer>(*h_->runtime_b);
    seed_->Start();
  }
  void TearDown() override {
    if (seed_) {
      seed_->Stop();
    }
  }
  std::unique_ptr<DialBackServer> seed_;
};

TEST_F(DialBackVersionCompatTest, AV9ProbeIsStillServed) {
  DialBackClient client(*h_->runtime_a, [this] { h_->PumpBoth(); });
  client.Start();
  auto probed = client.Probe("server", {h_->ma_a}, 8000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_TRUE(probed->ok) << probed->error;
  EXPECT_EQ(probed->refusal, DialBackRefusal::Ok);
  client.Stop();

  // By hand, as v9.0.0 sends it: the answer carries what it always did (and no code, being ok).
  const RawJsonReplies replies = Exchange(
      kDialBackProtocolId, {R"({"v":1,"op":"probe","target_multiaddrs":[")" + h_->ma_a + R"("],"timeout_ms":2000})"});
  ASSERT_EQ(replies.frames.size(), 1u);
  const common::Object reply = Parse(replies.frames.front());
  EXPECT_TRUE(reply.getIf<bool>("ok").value_or(false)) << replies.frames.front();
  EXPECT_EQ(reply.getString("dialed").value_or(""), h_->ma_a);
  EXPECT_FALSE(reply.contains("code"));
}

TEST_F(DialBackVersionCompatTest, ANewerVersionIsRefusedByCodeNamingOurs) {
  const RawJsonReplies replies = Exchange(
      kDialBackProtocolId, {R"({"v":2,"op":"probe","target_multiaddrs":[")" + h_->ma_a + R"("],"timeout_ms":2000})"});
  ASSERT_EQ(replies.frames.size(), 1u);
  const common::Object reply = Parse(replies.frames.front());
  EXPECT_FALSE(reply.getIf<bool>("ok").value_or(true));
  EXPECT_EQ(reply.getString("code").value_or(""), "bad_version") << replies.frames.front();
  EXPECT_EQ(SupportedVersionsText(reply), "1");
  EXPECT_FALSE(reply.getString("observed").value_or("").empty()) << "what it observed is still said";
}

TEST_F(DialBackVersionCompatTest, RefusalsCarryACode) {
  const RawJsonReplies unknown_op = Exchange(kDialBackProtocolId, {R"({"v":1,"op":"frobnicate"})"});
  ASSERT_EQ(unknown_op.frames.size(), 1u);
  EXPECT_EQ(Parse(unknown_op.frames.front()).getString("code").value_or(""), "bad_request");
  EXPECT_FALSE(Parse(unknown_op.frames.front()).getString("error").value_or("").empty()) << "the text stays";

  // A probe that ran and reached nothing: not_reached, read by the client as its refusal.
  DialBackClient client(*h_->runtime_a, [this] { h_->PumpBoth(); });
  client.Start();
  auto probed = client.Probe("server", {"/ip4/203.0.113.1/tcp/39999/p2p/" + h_->peer_id_a}, 2000);
  ASSERT_TRUE(static_cast<bool>(probed)) << probed.error().message;
  EXPECT_FALSE(probed->ok);
  EXPECT_EQ(probed->refusal, DialBackRefusal::NotReached);
  client.Stop();
}

TEST_F(ReachVersionCompatTest, ASeedsBadVersionIsAVersionMismatchAndAV9RefusalStaysAResult) {
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> sessions;
  std::string answer;
  h_->runtime_b->SetProtocolHandler(
      kDialBackProtocolId, [&](pp::amp::LinkHandle, const std::string& remote_peer_id, const uint32_t channel_id) {
        auto holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
        *holder = h_->mgr_b().BindChannel(
            remote_peer_id, channel_id, pp::amp::ControlJsonChannelPolicy(),
            [holder, &answer](Roe<std::vector<uint8_t>> frame) {
              if (frame && *holder) {
                (void)(*holder)->EnqueueOutbound(std::vector<uint8_t>(answer.begin(), answer.end()));
              }
              return true;
            });
        sessions.push_back(*holder);
      });
  DialBackClient client(*h_->runtime_a, [this] { h_->PumpBoth(); });
  client.Start();

  answer = R"({"v":1,"ok":false,"dialed":"","observed":"","code":"bad_version","supported_versions":[1],"error":"x"})";
  auto refused = client.Probe("server", {h_->ma_a}, 2000);
  ASSERT_FALSE(static_cast<bool>(refused));
  EXPECT_EQ(refused.error().GetCode(), DialBackErr::VersionMismatch) << refused.error().message;

  answer = R"({"v":1,"ok":false,"dialed":"","observed":"","error":"unsupported op"})"; // v9.0.0: text only
  auto plain = client.Probe("server", {h_->ma_a}, 2000);
  ASSERT_TRUE(static_cast<bool>(plain)) << plain.error().message;
  EXPECT_FALSE(plain->ok);
  EXPECT_EQ(plain->refusal, DialBackRefusal::Generic);
  EXPECT_EQ(plain->error, "unsupported op");

  client.Stop();
  h_->mgr_b().RemoveProtocolHandler(kDialBackProtocolId);
}

TEST(DialBackWireTest, CodesRoundTrip) {
  for (const DialBackRefusal refusal :
       {DialBackRefusal::BadRequest, DialBackRefusal::Busy, DialBackRefusal::NotReached, DialBackRefusal::BadVersion}) {
    EXPECT_EQ(DialBackRefusalFromCode(DialBackRefusalWire(refusal)), refusal) << DialBackRefusalWire(refusal);
  }
  EXPECT_EQ(DialBackRefusalFromCode(""), DialBackRefusal::Generic);
  EXPECT_EQ(DialBackRefusalFromCode("from_the_future"), DialBackRefusal::Generic);
}

} // namespace
} // namespace pp::mesh
