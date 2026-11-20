// Release compatibility (pp-browser projects/release-compatibility U7) for /pp-mesh/directory/1.0.0: a v9.0.0
// request (`version`:1) is served exactly as before; a version mismatch is a refusal naming the versions served,
// which the client reads as VersionMismatch (update needed), not as a protocol error.

#include "mesh/discovery/AmpDirectoryProtocol.h"

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

class DirectoryVersionCompatTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    h_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(h_->mgr_a().RegisterEndpoint("seed", h_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(h_->mgr_b().RegisterEndpoint("client", h_->ma_a)));
  }

  common::Object Ask(const std::string& request) {
    const test::RawJsonReplies replies =
        test::RawJsonExchange(h_->mgr_a(), [this] { h_->PumpBoth(); }, "seed", kDirectoryProtocolId, {request});
    EXPECT_EQ(replies.frames.size(), 1u) << request;
    auto root = replies.frames.empty() ? std::nullopt : common::TryParseObject(replies.frames.front());
    return root ? *root : common::Object{};
  }

  std::unique_ptr<test::AmpMeshHarness> h_;
};

TEST_F(DirectoryVersionCompatTest, AV9RequestIsServedAndAMismatchNamesOurVersions) {
  auto pump = [this]() { h_->PumpBoth(); };
  AmpDirectoryProtocol seed(*h_->runtime_b, pump);
  AmpDirectoryProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = h_->peer_id_b;
  seed.Configure(seed_cfg);
  MeshNodeHit published;
  published.account_id = "account:seed";
  published.entity_kind = "mesh_node";
  seed.SetNodesSnapshot({published});
  seed.Start();

  const common::Object listed = Ask(R"({"op":"list_mesh_nodes","req_id":"r1","version":1})");
  EXPECT_EQ(listed.getString("op").value_or(""), "list_mesh_nodes_result");
  ASSERT_NE(listed.getArray("nodes"), nullptr);
  EXPECT_EQ(listed.getArray("nodes")->elements.size(), 1u);

  const common::Object newer = Ask(R"({"op":"list_mesh_nodes","req_id":"r2","version":2})");
  EXPECT_EQ(newer.getString("op").value_or(""), "error");
  EXPECT_EQ(newer.getString("code").value_or(""), "bad_version");
  EXPECT_EQ(SupportedVersionsText(newer), "1");
  seed.Stop();
}

TEST_F(DirectoryVersionCompatTest, ASeedsBadVersionIsAVersionMismatch) {
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> sessions;
  std::string code = "bad_version";
  h_->runtime_b->SetProtocolHandler(
      kDirectoryProtocolId, [&](pp::amp::LinkHandle, const std::string& remote_peer_id, const uint32_t channel_id) {
        auto holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
        *holder = h_->mgr_b().BindChannel(
            remote_peer_id, channel_id, pp::amp::ControlJsonChannelPolicy(),
            [holder, &code](Roe<std::vector<uint8_t>> frame) {
              if (frame && *holder) {
                const std::string answer = R"({"op":"error","req_id":"","version":1,"code":")" + code +
                                           R"(","message":"no","supported_versions":[2]})";
                (void)(*holder)->EnqueueOutbound(std::vector<uint8_t>(answer.begin(), answer.end()));
              }
              return true;
            });
        sessions.push_back(*holder);
      });
  auto pump = [this]() { h_->PumpBoth(); };
  AmpDirectoryProtocol client(*h_->runtime_a, pump);
  AmpDirectoryProtocolConfig client_cfg;
  client_cfg.local_peer_id = h_->peer_id_a;
  client_cfg.query_peer_keys = {"seed"};
  client.Configure(client_cfg);
  client.Start();

  auto listed = client.ListMeshNodes();
  ASSERT_FALSE(static_cast<bool>(listed));
  EXPECT_EQ(listed.error().GetCode(), DirectoryClient::Err::VersionMismatch) << listed.error().message;
  EXPECT_NE(listed.error().message.find("it reads 2"), std::string::npos) << listed.error().message;

  code = "rate_limited"; // any other refusal stays a protocol error
  auto limited = client.ListMeshNodes();
  ASSERT_FALSE(static_cast<bool>(limited));
  EXPECT_EQ(limited.error().GetCode(), DirectoryClient::Err::ProtocolError);

  client.Stop();
  h_->mgr_b().RemoveProtocolHandler(kDirectoryProtocolId);
}

} // namespace
} // namespace pp::mesh
