#include "mesh/host/MeshPorts.h"

#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/WireCodec.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/link/AdpMultiaddr.h"
#include "mesh/host/AmpLinkErr.h"
#include "mesh/testing/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace pp::mesh::test {
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
  harness_->chat_a().OpenChannel("missing-peer", "/test/rpc/chat/1.0.0", pp::amp::ControlJsonChannelPolicy(),
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

  harness_->runtime_a->Links().EstablishNestedOverCarrier("b", nullptr, true,
                                                          [&](auto result) { mgr_result = std::move(result); });
  harness_->chat_a().EstablishNestedOverCarrier(
      "b", nullptr, true, [&](IChatPeerLinks::LinkRoe result) { port_result = std::move(result); });
  harness_->PumpBoth();

  ASSERT_FALSE(static_cast<bool>(mgr_result));
  ASSERT_FALSE(static_cast<bool>(port_result));
  EXPECT_EQ(port_result.error().GetCode(), IChatPeerLinks::Err::NestedCarrierIncomplete);
  EXPECT_EQ(static_cast<int32_t>(port_result.error().GetCode()), static_cast<int32_t>(mgr_result.error().GetCode()));
}

// A dial to a node of another wire version: it answers every datagram with version negotiation (N032), so
// Amp fails the dial as VersionMismatch. The port reports it as that — not Generic, the code at the same
// number before the mirror had VersionMismatch.
TEST_F(MeshPortsTest, ChatPortReportsAnotherWireVersionAsVersionMismatch) {
  const auto addr_c = pp::adp::IpEndpoint::V4(10, 0, 0, 3, 3000);
  pp::adp::MemoryDatagramIo carol(harness_->hub, addr_c);
  const auto ma_c = pp::amp::FormatAdpMultiaddr(addr_c, harness_->peer_id_b);
  ASSERT_TRUE(static_cast<bool>(ma_c));
  ASSERT_TRUE(static_cast<bool>(harness_->chat_a().RegisterEndpoint("carol", *ma_c)));

  std::optional<IChatPeerLinks::LinkRoe> result;
  harness_->chat_a().EnsureAssociation("carol", [&](IChatPeerLinks::LinkRoe r) { result = std::move(r); });
  harness_->PumpUntil([&] {
    while (auto got = carol.RecvFrom()) {
      if (!got->has_value()) {
        break;
      }
      static constexpr uint8_t kOtherWire[] = {3};
      (void)carol.SendTo((*got)->first, pp::adp::WireCodec::EncodeVersionNegotiation((*got)->second, kOtherWire));
    }
    return result.has_value();
  });

  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(static_cast<bool>(*result));
  EXPECT_EQ(result->error().GetCode(), IChatPeerLinks::Err::VersionMismatch) << result->error().message;
  EXPECT_TRUE(IChatPeerLinks::IsVersionMismatch(result->error()));
  EXPECT_NE(result->error().message.find("wire v3"), std::string::npos) << "Amp's text, as it was";
}

// Every Amp link code crosses into the port as the code of its name and value, and only Amp's Generic reads
// Generic. A code Amp adds without a mesh counterpart fails the build (ToMeshLinkErr names every one); one
// inserted before Generic moves Generic's value, which fails here and MeshPorts.cpp's static_asserts.
TEST(MeshLinkErrTest, EveryAmpCodeCrossesAsItself) {
  using Amp = pp::amp::PeerLinkManager::Err;
  const auto last = static_cast<int32_t>(Amp::Generic);
  for (int32_t value = 0; value <= last; ++value) {
    const IChatPeerLinks::Err code = ToMeshLinkErr(static_cast<Amp>(value));
    EXPECT_EQ(static_cast<int32_t>(code), value);
    EXPECT_EQ(code == IChatPeerLinks::Err::Generic, value == last) << "Amp code " << value << " reads Generic";
  }
  EXPECT_EQ(ToMeshLinkErr(Amp::VersionMismatch), IChatPeerLinks::Err::VersionMismatch);
  EXPECT_EQ(ToMeshLinkErr(Amp::Generic), IChatPeerLinks::Err::Generic);
  EXPECT_EQ(ToMeshLinkErr(static_cast<Amp>(last + 1)), IChatPeerLinks::Err::Generic) << "no Amp code: unclassified";
}

TEST(MeshLinkErrTest, AFailureKeepsAmpsMessage) {
  const auto failure = ToMeshLinkFailure(
      pp::amp::PeerLinkManager::Failure::Of(pp::amp::PeerLinkManager::Err::VersionMismatch, "the peer speaks wire v3"));
  EXPECT_EQ(failure.GetCode(), IChatPeerLinks::Err::VersionMismatch);
  EXPECT_EQ(failure.message, "the peer speaks wire v3");
}

} // namespace
} // namespace pp::mesh::test
