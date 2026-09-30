#include "common/privacy/AddressDisclosure.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

AddressDisclosurePolicy PolicyWith(const DirectAudience audience) {
  AddressDisclosurePolicy policy;
  policy.audience = audience;
  policy.contacts = {"QmContact", "QmFriend", "account:friend"};
  policy.friendly = {"QmFriend", "account:friend"};
  policy.blocked = {"QmBlocked"};
  return policy;
}

// projects/privacy P002: who may learn our address, per audience; Blocked never.
TEST(AddressDisclosureTest, AudienceDecidesWhoIsAllowed) {
  const auto everyone = PolicyWith(DirectAudience::Everyone);
  EXPECT_TRUE(everyone.AllowsDirect("QmStranger"));
  EXPECT_FALSE(everyone.AllowsDirect("QmBlocked")) << "Blocked is never allowed";

  const auto contacts = PolicyWith(DirectAudience::Contacts);
  EXPECT_TRUE(contacts.AllowsDirect("QmContact"));
  EXPECT_TRUE(contacts.AllowsDirect("QmFriend"));
  EXPECT_FALSE(contacts.AllowsDirect("QmStranger"));

  const auto friendly = PolicyWith(DirectAudience::Friendly);
  EXPECT_TRUE(friendly.AllowsDirect("QmFriend"));
  EXPECT_TRUE(friendly.AllowsDirect("account:friend")) << "an account id works like a PeerId";
  EXPECT_FALSE(friendly.AllowsDirect("QmContact"));

  const auto nobody = PolicyWith(DirectAudience::Nobody);
  EXPECT_FALSE(nobody.AllowsDirect("QmFriend"));

  EXPECT_FALSE(everyone.AllowsDirect("")) << "no identity, no disclosure";
}

TEST(AddressDisclosureTest, GateAllowsNothingUntilPublishedAndNullGateAllowsAll) {
  AddressDisclosureGate gate;
  EXPECT_FALSE(gate.AllowsDirect("QmContact"));
  gate.Publish(PolicyWith(DirectAudience::Contacts));
  EXPECT_TRUE(gate.AllowsDirect("QmContact"));
  EXPECT_FALSE(AllowsDirect(&gate, "QmStranger"));
  EXPECT_TRUE(AllowsDirect(nullptr, "QmStranger")) << "unwired callers disclose as before";
}

// privacy Y2: public records (the directory) carry our addresses only for an everyone audience.
TEST(AddressDisclosureTest, BlockedKeysAreKnown) {
  const auto policy = PolicyWith(DirectAudience::Everyone);
  EXPECT_TRUE(policy.IsBlocked("QmBlocked"));
  EXPECT_FALSE(policy.IsBlocked("QmStranger"));
  EXPECT_FALSE(policy.IsBlocked(""));
}

TEST(AddressDisclosureTest, OnlyEveryonePublishesAddresses) {
  EXPECT_TRUE(PublishesAddresses(DirectAudience::Everyone));
  EXPECT_FALSE(PublishesAddresses(DirectAudience::Contacts));
  EXPECT_FALSE(PublishesAddresses(DirectAudience::Friendly));
  EXPECT_FALSE(PublishesAddresses(DirectAudience::Nobody));
}

TEST(AddressDisclosureTest, AudienceNamesRoundTrip) {
  for (const auto audience : {DirectAudience::Everyone, DirectAudience::Contacts, DirectAudience::Friendly,
                              DirectAudience::Nobody}) {
    EXPECT_EQ(DirectAudienceFromName(DirectAudienceName(audience)), audience);
  }
  EXPECT_FALSE(DirectAudienceFromName("strangers").has_value());
}

} // namespace
} // namespace pbr
