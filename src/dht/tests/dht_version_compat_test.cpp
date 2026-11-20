// Release compatibility (pp-browser projects/release-compatibility V3, T11, U7) for /pp-mesh/dht/1.0.0 and its
// PeerRoutingRecord: a v9.0.0 request (`version`:1) and record are served exactly as before; a version mismatch is a
// refusal that names the versions served, which the client reads as VersionMismatch; a record this build cannot read
// is skipped as "update needed" and never counted against the peer that served it; a publish every peer refused
// backs off instead of retrying every 10 s.

#include "mesh/dht/AmpDhtProtocol.h"
#include "mesh/dht/DhtRecordCodec.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "crypto/SodiumUtil.h"
#include "mesh/l4/shared/WireVersion.h"
#include "mesh/testing/mesh_test_harness.h"
#include "mesh/testing/raw_json_exchange.h"

#include <gtest/gtest.h>

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pp::mesh {
namespace {

common::Object Parse(const std::string& json) {
  auto root = common::TryParseObject(json);
  EXPECT_TRUE(root.has_value()) << json;
  return root ? *root : common::Object{};
}

PeerRoutingRecord SignedRecord(const std::string& peer_id, const std::string& multiaddr,
                               const pp::amp::MshIdentity& identity) {
  PeerRoutingRecord record;
  record.peer_id = peer_id;
  record.seq = 1;
  record.ttl_seconds = 3600;
  record.issued_at = static_cast<int64_t>(std::time(nullptr));
  record.multiaddrs = {multiaddr};
  auto signed_record = SignPeerRoutingRecord(record, identity.ml_dsa_secret_key);
  EXPECT_TRUE(static_cast<bool>(signed_record));
  signed_record->public_key_b64 = pp::Base64Encode(identity.ml_dsa_public_key);
  return *signed_record;
}

// ---- the record (V3) ----

TEST(DhtRecordVersionTest, AV9RecordIsSupportedAndOthersAreUpdateNeeded) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWTestPeer";
  record.seq = 1;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/12D3KooWTestPeer"};
  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));
  const common::Object v9 = PeerRoutingRecordToObject(*signed_record);
  EXPECT_EQ(v9.getIf<int64_t>("version").value_or(0), 1) << "what v9.0.0 writes, unchanged";
  EXPECT_EQ(PeerRoutingRecordSupportOf(v9), PeerRoutingRecordSupport::Supported);
  auto parsed = PeerRoutingRecordFromObject(v9);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;
  auto verified = VerifyPeerRoutingRecord(*parsed, keys->public_key);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);

  common::Object no_version = v9;
  no_version.erase("version");
  EXPECT_EQ(PeerRoutingRecordSupportOf(no_version), PeerRoutingRecordSupport::Supported) << "absent reads as 1";

  common::Object newer = v9;
  newer.set("version", int64_t{2});
  EXPECT_EQ(PeerRoutingRecordSupportOf(newer), PeerRoutingRecordSupport::UnsupportedVersion);
  EXPECT_FALSE(static_cast<bool>(PeerRoutingRecordFromObject(newer)));

  common::Object odd = v9;
  odd.set("version", "2.0");
  EXPECT_EQ(PeerRoutingRecordSupportOf(odd), PeerRoutingRecordSupport::UnsupportedVersion);

  common::Object other_alg = v9;
  other_alg.set("signature_alg", "ml-dsa-87");
  EXPECT_EQ(PeerRoutingRecordSupportOf(other_alg), PeerRoutingRecordSupport::UnsupportedSignatureAlg);
  EXPECT_FALSE(static_cast<bool>(PeerRoutingRecordFromObject(other_alg)));
}

// ---- the server (U7, V3) ----

class DhtVersionCompatTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    h_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(h_->mgr_a().RegisterEndpoint("seed", h_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(h_->mgr_b().RegisterEndpoint("client", h_->ma_a)));
  }

  AmpDhtProtocolConfig ClientConfig(const bool participate) const {
    AmpDhtProtocolConfig cfg;
    cfg.local_peer_id = h_->peer_id_a;
    cfg.listen_multiaddrs = {h_->ma_a};
    cfg.device_signing_secret = h_->alice.ml_dsa_secret_key;
    cfg.device_signing_public = h_->alice.ml_dsa_public_key;
    cfg.query_peer_keys = {"seed"};
    cfg.participate = participate;
    return cfg;
  }

  std::string Ask(const std::string& request) {
    const test::RawJsonReplies replies =
        test::RawJsonExchange(h_->mgr_a(), [this] { h_->PumpBoth(); }, "seed", kDhtProtocolId, {request});
    EXPECT_EQ(replies.frames.size(), 1u) << request;
    return replies.frames.empty() ? std::string() : replies.frames.front();
  }

  /** B answers every DHT request with `answer(request)`. */
  void FakeSeed(std::function<std::string(const common::Object&)> answer) {
    h_->runtime_b->SetProtocolHandler(kDhtProtocolId, [this, answer = std::move(answer)](
                                                          pp::amp::LinkHandle, const std::string& remote_peer_id,
                                                          const uint32_t channel_id) {
      auto holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
      *holder = h_->mgr_b().BindChannel(remote_peer_id, channel_id, pp::amp::ControlJsonChannelPolicy(),
                                        [holder, answer](Roe<std::vector<uint8_t>> frame) {
                                          if (frame && *holder) {
                                            const std::string reply =
                                                answer(Parse(std::string(frame->begin(), frame->end())));
                                            (void)(*holder)->EnqueueOutbound(
                                                std::vector<uint8_t>(reply.begin(), reply.end()));
                                          }
                                          return true;
                                        });
      fake_sessions_.push_back(*holder);
    });
  }

  AmpDhtProtocol::FindPeerRoe Find(AmpDhtProtocol& client, const std::string& target) {
    SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait;
    client.FindPeer(target, [&wait](AmpDhtProtocol::FindPeerRoe result) { wait.Finish(std::move(result)); });
    h_->PumpUntil([&]() { return wait.IsSettled(); }, 3000);
    return wait.Wait(std::chrono::seconds(5), AmpDhtProtocol::Failure::Of(AmpDhtProtocol::Err::Timeout, "unsettled"));
  }

  std::unique_ptr<test::AmpMeshHarness> h_;
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> fake_sessions_;
};

TEST_F(DhtVersionCompatTest, AV9RequestIsServedAndAMismatchNamesOurVersions) {
  AmpDhtProtocol seed(*h_->runtime_b);
  AmpDhtProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = h_->peer_id_b;
  seed_cfg.participate = true;
  seed.Configure(seed_cfg);
  seed.Start();

  const common::Object pong = Parse(Ask(R"({"op":"ping","req_id":"r1","version":1})"));
  EXPECT_EQ(pong.getString("op").value_or(""), "pong");
  EXPECT_EQ(pong.getString("peer_id").value_or(""), h_->peer_id_b);

  const common::Object newer = Parse(Ask(R"({"op":"ping","req_id":"r2","version":2})"));
  EXPECT_EQ(newer.getString("op").value_or(""), "error");
  EXPECT_EQ(newer.getString("code").value_or(""), "bad_version");
  EXPECT_EQ(newer.getString("req_id").value_or(""), "r2");
  EXPECT_EQ(SupportedVersionsText(newer), "1");

  // As v9.0.0 did: a request without a version is refused too (the check stays strict, and first).
  const common::Object none = Parse(Ask(R"({"op":"find_peer","req_id":"r3","peer_id":"x"})"));
  EXPECT_EQ(none.getString("code").value_or(""), "bad_version");
  seed.Stop();
}

TEST_F(DhtVersionCompatTest, AStoreOfARecordWeCannotReadIsATypedBadVersion) {
  AmpDhtProtocol seed(*h_->runtime_b);
  AmpDhtProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = h_->peer_id_b;
  seed_cfg.participate = true;
  seed.Configure(seed_cfg);
  seed.Start();

  const PeerRoutingRecord record = SignedRecord(h_->peer_id_a, h_->ma_a, h_->alice);
  const auto store = [](const common::Object& record_obj) {
    common::Object request;
    request.set("op", "store");
    request.set("req_id", "s");
    request.set("version", int64_t{1});
    request.set("record", record_obj);
    return common::DumpJson(request);
  };

  common::Object newer = PeerRoutingRecordToObject(record);
  newer.set("version", int64_t{2});
  const common::Object refused = Parse(Ask(store(newer)));
  EXPECT_EQ(refused.getString("code").value_or(""), "bad_version") << common::DumpJson(refused);
  EXPECT_EQ(SupportedVersionsText(refused, "record_versions"), "1");
  ASSERT_NE(refused.getArray("signature_algs"), nullptr);

  common::Object other_alg = PeerRoutingRecordToObject(record);
  other_alg.set("signature_alg", "ml-dsa-87");
  EXPECT_EQ(Parse(Ask(store(other_alg))).getString("code").value_or(""), "bad_version") << "not bad_signature";
  EXPECT_FALSE(seed.LocalRecord(h_->peer_id_a).has_value());

  // The record as v9.0.0 publishes it: stored.
  const common::Object stored = Parse(Ask(store(PeerRoutingRecordToObject(record))));
  EXPECT_EQ(stored.getString("op").value_or(""), "store_result") << common::DumpJson(stored);
  EXPECT_TRUE(stored.getIf<bool>("ok").value_or(false));
  EXPECT_TRUE(seed.LocalRecord(h_->peer_id_a).has_value());
  seed.Stop();
}

// ---- the client (U7, V3, T11) ----

TEST_F(DhtVersionCompatTest, ASeedsBadVersionIsAVersionMismatchNotAProtocolError) {
  FakeSeed([](const common::Object& request) {
    return R"({"op":"error","req_id":")" + request.getString("req_id").value_or("") +
           R"(","version":1,"code":"bad_version","message":"unsupported dht version 1","supported_versions":[2]})";
  });
  AmpDhtProtocol client(*h_->runtime_a);
  client.Configure(ClientConfig(false));
  client.Start();
  auto found = Find(client, "QmSomeone");
  ASSERT_FALSE(static_cast<bool>(found));
  EXPECT_EQ(found.error().GetCode(), DhtClient::Err::VersionMismatch) << found.error().message;
  EXPECT_NE(found.error().message.find("it reads 2"), std::string::npos) << found.error().message;

  // Any other error code stays what it was.
  EXPECT_EQ(DhtClient::FailureFromErrorReply(Parse(R"({"op":"error","code":"rate_limited"})"), "find_peer").GetCode(),
            DhtClient::Err::ProtocolError);
  client.Stop();
  h_->mgr_b().RemoveProtocolHandler(kDhtProtocolId);
}

TEST_F(DhtVersionCompatTest, APeerThatServesNoDhtIsNotServed) {
  h_->mgr_b().SetRefuseUnhandledOpens(true); // as a product host does: no handler, OpenAck "no handler"
  AmpDhtProtocol client(*h_->runtime_a);
  client.Configure(ClientConfig(false));
  client.Start();
  auto found = Find(client, "QmSomeone");
  ASSERT_FALSE(static_cast<bool>(found));
  EXPECT_EQ(found.error().GetCode(), DhtClient::Err::NotServed) << found.error().message;
  client.Stop();
}

// A record the client cannot read is "update needed": skipped, and the peer that served it is not penalized — unlike
// a malformed one, whose third strike puts the peer in its cooldown.
TEST_F(DhtVersionCompatTest, ARecordWeCannotReadIsSkippedAndNotCountedAgainstThePeer) {
  const auto answer_with = [this](const std::function<void(common::Object&)>& edit) {
    FakeSeed([this, edit](const common::Object& request) {
      const std::string target = request.getString("peer_id").value_or("");
      common::Object record = PeerRoutingRecordToObject(SignedRecord(h_->peer_id_a, h_->ma_a, h_->alice));
      record.set("peer_id", target);
      edit(record);
      common::Object reply;
      reply.set("op", "find_peer_result");
      reply.set("req_id", request.getString("req_id").value_or(""));
      reply.set("version", int64_t{1});
      reply.set("peer_id", target);
      reply.set("records", common::makeArray({common::Value(std::make_shared<common::Object>(record))}));
      reply.set("closer_peers", common::makeArray({}));
      return common::DumpJson(reply);
    });
  };

  answer_with([](common::Object& record) { record.set("version", int64_t{2}); });
  AmpDhtProtocol client(*h_->runtime_a);
  client.Configure(ClientConfig(false));
  client.Start();
  for (int i = 0; i < 4; ++i) { // past the default penalty threshold (3)
    auto found = Find(client, "QmNewer" + std::to_string(i));
    ASSERT_FALSE(static_cast<bool>(found));
    EXPECT_EQ(found.error().GetCode(), DhtClient::Err::NotFound) << found.error().message;
  }
  EXPECT_EQ(client.Stats().records_update_needed, 4u);
  EXPECT_EQ(client.Stats().soft_reputation_penalized_peers, 0u) << "update needed is not misbehaviour";

  answer_with([](common::Object& record) { record.set("signature_alg", "ml-dsa-87"); });
  auto found = Find(client, "QmOtherAlg");
  ASSERT_FALSE(static_cast<bool>(found));
  EXPECT_EQ(client.Stats().records_update_needed, 5u);
  EXPECT_EQ(client.Stats().soft_reputation_penalized_peers, 0u);

  // Control: a malformed record (its multiaddr names another peer) is misbehaviour, as before.
  answer_with([](common::Object& record) { record.erase("public_key_b64"); });
  for (int i = 0; i < 3; ++i) {
    (void)Find(client, "QmMalformed" + std::to_string(i));
  }
  EXPECT_EQ(client.Stats().soft_reputation_penalized_peers, 1u);
  client.Stop();
  h_->mgr_b().RemoveProtocolHandler(kDhtProtocolId);
}

TEST(DhtPublishBackoffTest, ARefusedPublishWaitsLongerEachTimeUpToTheRepublish) {
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(0, 3600), DhtClient::kPublishRetrySeconds);
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(10, 3600), 20);
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(20, 3600), 40);
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(1280, 3600), 1800) << "at most ttl / 2";
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(1800, 3600), 1800);
  EXPECT_EQ(DhtClient::NextPublishBackoffSeconds(20, 60), 30) << "a short ttl: its republish floor (30 s)";
  EXPECT_TRUE(DhtClient::IsRefusal(DhtClient::Err::VersionMismatch));
  EXPECT_TRUE(DhtClient::IsRefusal(DhtClient::Err::NotServed));
  EXPECT_FALSE(DhtClient::IsRefusal(DhtClient::Err::Timeout)) << "silence is retried soon, as before";
  EXPECT_FALSE(DhtClient::IsRefusal(DhtClient::Err::ChannelFailed));
}

// T11: a publish every peer refused (a version it does not read, or no DHT) backs off; one that is heard does not.
TEST_F(DhtVersionCompatTest, APublishEveryPeerRefusedBacksOff) {
  const auto publish_once = [this](AmpDhtProtocol& client) {
    client.Configure(ClientConfig(true));
    client.Start();
    client.Tick();
    h_->PumpUntil([&]() { return client.Stats().self_publishes >= 1; }, 3000);
    EXPECT_GE(client.Stats().self_publishes, 1u);
    h_->PumpUntil([]() { return false; }, 50); // the stores settle
    const int64_t backoff = client.Stats().publish_backoff_seconds;
    client.Stop();
    return backoff;
  };

  FakeSeed([](const common::Object& request) {
    return R"({"op":"error","req_id":")" + request.getString("req_id").value_or("") +
           R"(","version":1,"code":"bad_version","message":"x","supported_versions":[2]})";
  });
  {
    AmpDhtProtocol client(*h_->runtime_a);
    EXPECT_EQ(publish_once(client), DhtClient::kPublishRetrySeconds) << "refused: the backoff starts";
  }
  h_->mgr_b().RemoveProtocolHandler(kDhtProtocolId);

  h_->mgr_b().SetRefuseUnhandledOpens(true);
  {
    AmpDhtProtocol client(*h_->runtime_a);
    EXPECT_EQ(publish_once(client), DhtClient::kPublishRetrySeconds) << "not served: the same";
  }

  // A v9-style seed that answers: no backoff.
  AmpDhtProtocol seed(*h_->runtime_b);
  AmpDhtProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = h_->peer_id_b;
  seed_cfg.participate = true;
  seed.Configure(seed_cfg);
  seed.Start();
  {
    AmpDhtProtocol client(*h_->runtime_a);
    EXPECT_EQ(publish_once(client), 0) << "heard";
  }
  seed.Stop();
}

} // namespace
} // namespace pp::mesh
