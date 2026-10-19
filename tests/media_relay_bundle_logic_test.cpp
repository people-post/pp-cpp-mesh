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
  ASSERT_TRUE(book.Add(QuoteWithId("q1"), "call:a", "peer-a", t0));
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
  ASSERT_TRUE(book.Add(QuoteWithId("probe-1"), "call:a", "peer-a", t0));
  ASSERT_TRUE(book.Add(QuoteWithId("probe-2"), "call:b", "peer-a", t0 + std::chrono::seconds(30)));
  book.Expire(t0 + std::chrono::seconds(59));
  EXPECT_EQ(book.size(), 2u);
  book.Expire(t0 + std::chrono::seconds(60));
  EXPECT_EQ(book.size(), 1u) << "probe-1 expired";
  EXPECT_FALSE(book.Take("probe-2", t0 + std::chrono::seconds(95))) << "an expired quote cannot be accepted";
  EXPECT_EQ(book.size(), 0u);
}

TEST(MediaRelayQuoteBookTest, FullBookRefusesUntilQuotesExpire) {
  MediaRelayQuoteBook book(std::chrono::seconds(60), 2, 2);
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("a"), "call:a", "peer-a", t0));
  ASSERT_TRUE(book.Add(QuoteWithId("b"), "call:a", "peer-a", t0));
  EXPECT_FALSE(book.Add(QuoteWithId("c"), "call:a", "peer-a", t0 + std::chrono::seconds(1))) << "capped";
  EXPECT_TRUE(book.Add(QuoteWithId("c"), "call:a", "peer-a", t0 + std::chrono::seconds(61))) << "room after expiry";
  EXPECT_EQ(book.size(), 1u);
}

// One requester holding its share cannot fill the book for everyone else.
TEST(MediaRelayQuoteBookTest, PerRequesterShareLeavesRoomForOthers) {
  MediaRelayQuoteBook book(std::chrono::seconds(60), 10, 2);
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("a1"), "call:a", "peer-a", t0));
  ASSERT_TRUE(book.Add(QuoteWithId("a2"), "call:a", "peer-a", t0));
  EXPECT_FALSE(book.Add(QuoteWithId("a3"), "call:a", "peer-a", t0)) << "peer-a is at its share";
  EXPECT_TRUE(book.Add(QuoteWithId("b1"), "call:b", "peer-b", t0)) << "others still get quotes";
  ASSERT_TRUE(book.Take("a1", t0));
  EXPECT_TRUE(book.Add(QuoteWithId("a3"), "call:a", "peer-a", t0)) << "an accept frees the share";
  book.Expire(t0 + std::chrono::seconds(60));
  EXPECT_EQ(book.size(), 0u);
  EXPECT_TRUE(book.Add(QuoteWithId("a4"), "call:a", "peer-a", t0 + std::chrono::seconds(60)))
      << "expiry frees the share";
}

// Admission runs between Find and Take: a refused accept must leave the quote in place.
TEST(MediaRelayQuoteBookTest, FindLeavesTheQuoteForTake) {
  MediaRelayQuoteBook book;
  const auto t0 = MediaRelayQuoteBook::Clock::time_point{};
  ASSERT_TRUE(book.Add(QuoteWithId("q1"), "call:a", "peer-a", t0));
  const auto* found = book.Find("q1", t0);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->call_id, "call:a");
  EXPECT_EQ(book.size(), 1u);
  EXPECT_EQ(book.Find("q1", t0 + std::chrono::seconds(60)), nullptr) << "expired";
  EXPECT_TRUE(book.Take("q1", t0));
}

} // namespace
} // namespace pbr
