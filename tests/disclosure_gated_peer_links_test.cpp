#include "domain/mesh/host/DisclosureGatedPeerLinks.h"

#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <optional>

namespace pbr {
namespace {

using Links = IChatPeerLinks;

std::optional<bool> Associate(test::AmpMeshHarness& harness, Links& links, const std::string& key,
                              std::optional<Links::Failure>* failure = nullptr) {
  std::optional<bool> done;
  links.EnsureAssociation(key, [&](Links::LinkRoe result) {
    done = static_cast<bool>(result);
    if (!result && failure) {
      *failure = result.error();
    }
  });
  harness.PumpUntil([&] { return done.has_value(); }, 2000);
  return done;
}

// projects/privacy T1: people-facing transports never dial a peer outside the audience — it learns
// our address only from a link that already exists (P003).
TEST(DisclosureGatedPeerLinksTest, DialsOnlyTheAudienceOrOverAnExistingLink) {
  auto created = test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto& harness = **created;
  harness.ep_a->SetAcceptEnabled(true);
  harness.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(harness.chat_links_a->RegisterEndpoint(harness.peer_id_b, harness.ma_b)));

  AddressDisclosureGate gate;
  AddressDisclosurePolicy contacts_only;
  contacts_only.audience = DirectAudience::Contacts;
  gate.Publish(contacts_only);
  DisclosureGatedPeerLinks gated(*harness.chat_links_a, gate);

  EXPECT_FALSE(gated.IsReachable(harness.peer_id_b)) << "an address is booked, but not one we may dial";
  EXPECT_FALSE(gated.GetLinkSnapshot(harness.peer_id_b).has_endpoint);
  std::optional<Links::Failure> failure;
  EXPECT_EQ(Associate(harness, gated, harness.peer_id_b, &failure), std::optional<bool>(false));
  ASSERT_TRUE(failure.has_value());
  EXPECT_TRUE(Links::IsEndpointNotRegistered(*failure));
  EXPECT_FALSE(harness.chat_links_a->IsConnected(harness.peer_id_b)) << "nothing was dialed";

  // The peer dials us (it had our address): that link may carry our traffic.
  ASSERT_TRUE(static_cast<bool>(harness.chat_links_b->RegisterEndpoint(harness.peer_id_a, harness.ma_a)));
  ASSERT_EQ(Associate(harness, *harness.chat_links_b, harness.peer_id_a), std::optional<bool>(true));
  harness.PumpUntil([&] { return harness.chat_links_a->IsConnected(harness.peer_id_b); }, 2000);
  ASSERT_TRUE(harness.chat_links_a->IsConnected(harness.peer_id_b));
  EXPECT_EQ(gated.IsReachable(harness.peer_id_b), harness.chat_links_a->IsReachable(harness.peer_id_b));
  EXPECT_TRUE(gated.GetLinkSnapshot(harness.peer_id_b).has_endpoint);
  EXPECT_EQ(Associate(harness, gated, harness.peer_id_b), std::optional<bool>(true));
}

TEST(DisclosureGatedPeerLinksTest, AllowedPeerDialsAsBefore) {
  auto created = test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto& harness = **created;
  harness.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(harness.chat_links_a->RegisterEndpoint(harness.peer_id_b, harness.ma_b)));

  AddressDisclosureGate gate;
  AddressDisclosurePolicy contacts;
  contacts.audience = DirectAudience::Contacts;
  contacts.contacts.insert(harness.peer_id_b);
  gate.Publish(contacts);
  DisclosureGatedPeerLinks gated(*harness.chat_links_a, gate);

  EXPECT_EQ(gated.IsReachable(harness.peer_id_b), harness.chat_links_a->IsReachable(harness.peer_id_b));
  EXPECT_TRUE(gated.GetLinkSnapshot(harness.peer_id_b).has_endpoint);
  EXPECT_EQ(Associate(harness, gated, harness.peer_id_b), std::optional<bool>(true));
}

} // namespace
} // namespace pbr
