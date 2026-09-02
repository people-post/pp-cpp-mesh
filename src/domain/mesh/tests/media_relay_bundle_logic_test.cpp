#include "domain/mesh/l4/media_relay/MediaRelayBundleLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MediaRelayBundleLogicTest, AdmitAllowsEmptyContacts) {
  MediaRelayOpAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "quote";
  ctx.dialer_peer_id = "peer-a";
  EXPECT_EQ(DecideMediaRelayOpAdmit(ctx), MediaRelayOpAdmitDecision::Allow);
}

TEST(MediaRelayBundleLogicTest, AdmitRefusesStrangerUnlessCallScoped) {
  MediaRelayOpAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "attach";
  ctx.dialer_peer_id = "stranger";
  ctx.contact_peer_ids = {"friend"};
  ctx.serve_scope_mask = kRelayScopeLinkSiteSocial;
  EXPECT_EQ(DecideMediaRelayOpAdmit(ctx), MediaRelayOpAdmitDecision::RefuseStranger);

  ctx.session_exists_for_call = true;
  EXPECT_EQ(DecideMediaRelayOpAdmit(ctx), MediaRelayOpAdmitDecision::Allow);
}

TEST(MediaRelayBundleLogicTest, QuoteAndAttachAck) {
  EXPECT_EQ(DecideMediaRelayQuoteAck({.phase = MediaRelayBundlePhase::WaitQuote, .ack_ok = true}),
            MediaRelayQuoteAckDecision::Succeed);
  EXPECT_EQ(DecideMediaRelayAttachAck({.phase = MediaRelayBundlePhase::WaitAttachAck, .ack_ok = true}),
            MediaRelayAttachAckDecision::EnterAttached);
  EXPECT_EQ(DecideMediaRelayAttachAck({.phase = MediaRelayBundlePhase::Attached, .ack_ok = true}),
            MediaRelayAttachAckDecision::IgnoreStale);
}

TEST(MediaRelayBundleLogicTest, BuildDefaultQuote) {
  MediaRelayQuoteRequest req;
  req.call_id = "c1";
  req.want_up_bps = 1000;
  auto q = BuildDefaultMediaRelayQuote(req);
  EXPECT_TRUE(q.ok);
  EXPECT_FALSE(q.quote_id.empty());
  EXPECT_EQ(q.a_up_bps, 1000);
  EXPECT_EQ(q.pricing_mode, "volunteer");
}

} // namespace
} // namespace pbr
