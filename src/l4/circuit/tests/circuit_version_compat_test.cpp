// Release compatibility (pp-browser projects/release-compatibility V4) for /pp-mesh/circuit/1.0.0: a v9.0.0 request
// (`v`:1) is served exactly as before; a newer `v` is refused `bad_version` before admission, naming the versions
// served; the dialer reads that as VersionMismatch, not as a plain refusal.

#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"

#include "amp/L3/ChannelPolicy.h"
#include "common/ValueJson.h"
#include "mesh/l4/shared/ProductChannelPolicies.h"
#include "mesh/l4/shared/WireVersion.h"
#include "mesh/testing/mesh_triple_harness.h"
#include "mesh/testing/raw_json_exchange.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace pp::mesh {
namespace {

class CircuitVersionCompatTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    h_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(h_->mgr_a().RegisterEndpoint("relay", h_->ma_r)));
    relay_ = std::make_unique<CircuitRelayServer>(*h_->runtime_r);
    relay_->Start();
  }
  void TearDown() override {
    if (relay_) {
      relay_->Stop();
    }
  }

  test::RawJsonReplies Exchange(const std::string& request) {
    return test::RawJsonExchange(
        h_->mgr_a(), [this] { h_->PumpAll(); }, "relay", kCircuitRelayProtocolId, {request}, 1, 2000,
        pp::amp::CircuitTunnelChannelPolicy());
  }

  std::unique_ptr<test::AmpMeshTripleHarness> h_;
  std::unique_ptr<CircuitRelayServer> relay_;
};

TEST_F(CircuitVersionCompatTest, AV9ReserveIsStillServed) {
  for (const char* request : {R"({"v":1,"op":"reserve","timeout_ms":5000})", R"({"op":"reserve","timeout_ms":5000})"}) {
    const test::RawJsonReplies replies = Exchange(request);
    ASSERT_TRUE(replies.opened);
    ASSERT_EQ(replies.frames.size(), 1u) << request;
    auto ack = common::TryParseObject(replies.frames.front());
    ASSERT_TRUE(ack.has_value());
    EXPECT_TRUE(ack->getIf<bool>("ok").value_or(false)) << replies.frames.front();
    EXPECT_EQ(ack->getString("op").value_or(""), "reserve");
    EXPECT_EQ(ack->getIf<int64_t>("v").value_or(0), 1);
  }
}

TEST_F(CircuitVersionCompatTest, ANewerVersionIsRefusedByCodeNamingOurs) {
  const test::RawJsonReplies replies = Exchange(R"({"v":2,"op":"reserve","timeout_ms":5000})");
  ASSERT_EQ(replies.frames.size(), 1u);
  auto ack = common::TryParseObject(replies.frames.front());
  ASSERT_TRUE(ack.has_value());
  EXPECT_FALSE(ack->getIf<bool>("ok").value_or(true));
  EXPECT_EQ(ack->getString("code").value_or(""), "bad_version") << replies.frames.front();
  EXPECT_EQ(SupportedVersionsText(*ack), "1");
  EXPECT_EQ(CircuitRefusalFromAck(ack->getString("code").value_or("")), CircuitRefusal::BadVersion);
}

TEST(CircuitVersionCodesTest, BadVersionIsAVersionMismatchAndUnknownCodesStayGeneric) {
  EXPECT_EQ(CircuitRefusalFromAck("bad_version"), CircuitRefusal::BadVersion);
  EXPECT_EQ(CircuitRefusalFromAck("from_the_future"), CircuitRefusal::Generic);
  EXPECT_EQ(CircuitClientCoordinator::FromRefusal(CircuitRefusal::BadVersion, "x").GetCode(),
            CircuitTunnelErr::VersionMismatch);
  EXPECT_EQ(CircuitClientCoordinator::FromRefusal(CircuitRefusal::BadRequest, "x").GetCode(),
            CircuitTunnelErr::RelayRefused);
  for (const CircuitRefusal refusal : kCircuitRefusalReasons) {
    EXPECT_NE(refusal, CircuitRefusal::BadVersion) << "answered before admission: no bridge-failure series";
  }
}

} // namespace
} // namespace pp::mesh
