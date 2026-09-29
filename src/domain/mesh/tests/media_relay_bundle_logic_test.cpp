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
  req.session_id = "c1";
  req.want_up_bps = 1000;
  auto q = BuildDefaultMediaRelayQuote(req);
  EXPECT_TRUE(q.ok);
  EXPECT_FALSE(q.quote_id.empty());
  EXPECT_EQ(q.a_up_bps, 1000);
  EXPECT_EQ(q.pricing_mode, "volunteer");
}

// --- Host pending quotes (unaccepted quotes expire; the book is capped) --------------------------

MediaRelayQuote QuoteWithId(const std::string& id) {
  MediaRelayQuote q;
  q.ok = true;
  q.quote_id = id;
  return q;
}

TEST(MediaRelayQuoteBookTest, AcceptTakesTheQuoteOnce) {
  MediaRelayQuoteBook book;
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("q1"), "call:a", t0));
  auto taken = book.Take("q1", t0 + std::chrono::seconds(5));
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->call_id, "call:a");
  EXPECT_FALSE(book.Take("q1", t0 + std::chrono::seconds(6))) << "a quote is accepted once";
  EXPECT_EQ(book.size(), 0u);
}

// V050 invitees quote hops while ringing and never accept: those quotes must not stay forever.
TEST(MediaRelayQuoteBookTest, UnacceptedQuotesExpire) {
  MediaRelayQuoteBook book(std::chrono::seconds(60));
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("probe-1"), "call:a", t0));
  ASSERT_TRUE(book.Add(QuoteWithId("probe-2"), "call:b", t0 + std::chrono::seconds(30)));
  book.Expire(t0 + std::chrono::seconds(59));
  EXPECT_EQ(book.size(), 2u);
  book.Expire(t0 + std::chrono::seconds(60));
  EXPECT_EQ(book.size(), 1u) << "probe-1 expired";
  EXPECT_FALSE(book.Take("probe-2", t0 + std::chrono::seconds(95))) << "an expired quote cannot be accepted";
  EXPECT_EQ(book.size(), 0u);
}

TEST(MediaRelayQuoteBookTest, FullBookRefusesUntilQuotesExpire) {
  MediaRelayQuoteBook book(std::chrono::seconds(60), 2);
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("a"), "call:a", t0));
  ASSERT_TRUE(book.Add(QuoteWithId("b"), "call:a", t0));
  EXPECT_FALSE(book.Add(QuoteWithId("c"), "call:a", t0 + std::chrono::seconds(1))) << "capped";
  EXPECT_TRUE(book.Add(QuoteWithId("c"), "call:a", t0 + std::chrono::seconds(61))) << "room after expiry";
  EXPECT_EQ(book.size(), 1u);
}

} // namespace
} // namespace pbr
