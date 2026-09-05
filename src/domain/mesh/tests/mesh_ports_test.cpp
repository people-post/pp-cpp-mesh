#include "domain/mesh/host/MeshPorts.h"

#include "amp/L3/ChannelPolicy.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pbr::test {
namespace {

class MeshPortsTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);
  }

  std::unique_ptr<AmpMeshHarness> harness_;
};

TEST_F(MeshPortsTest, ChatPortPreservesEndpointNotRegisteredCode) {
  bool called = false;
  IChatPeerLinks::Err code = IChatPeerLinks::Err::Ok;
  harness_->chat_a().OpenChannel(
      "missing-peer", "/pp-browser/rpc/1.0.0", pp::amp::ControlJsonChannelPolicy(),
      [&](IChatPeerLinks::ChannelRoe result) {
        called = true;
        ASSERT_FALSE(static_cast<bool>(result));
        code = result.error().GetCode();
        EXPECT_TRUE(IChatPeerLinks::IsEndpointNotRegistered(result.error()));
      });
  harness_->PumpBoth();

  EXPECT_TRUE(called);
  EXPECT_EQ(code, IChatPeerLinks::Err::EndpointNotRegistered);
}

TEST_F(MeshPortsTest, ChatPortMatchesManagerLinkFailureCode) {
  pp::amp::PeerLinkManager::LinkRoe mgr_result;
  IChatPeerLinks::LinkRoe port_result;

  harness_->runtime_a->Links().EstablishNestedOverCarrier("b", nullptr, true, [&](auto result) {
    mgr_result = std::move(result);
  });
  harness_->chat_a().EstablishNestedOverCarrier("b", nullptr, true, [&](IChatPeerLinks::LinkRoe result) {
    port_result = std::move(result);
  });
  harness_->PumpBoth();

  ASSERT_FALSE(static_cast<bool>(mgr_result));
  ASSERT_FALSE(static_cast<bool>(port_result));
  EXPECT_EQ(port_result.error().GetCode(), IChatPeerLinks::Err::NestedCarrierIncomplete);
  EXPECT_EQ(static_cast<int32_t>(port_result.error().GetCode()),
            static_cast<int32_t>(mgr_result.error().GetCode()));
}

} // namespace
} // namespace pbr::test
