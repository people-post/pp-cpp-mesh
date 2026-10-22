// What the mesh remembers about reaching each peer, from Amp's link events (docs/PEER_REACH.md § 4).
#include "mesh/reach/PeerReachRecord.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

using amp::LinkDropReason;
using amp::LinkEvent;

constexpr uint64_t kNet = 1;

LinkEvent Connected(const std::string& peer, amp::LinkPathKind path = amp::LinkPathKind::Direct) {
  LinkEvent e;
  e.kind = LinkEvent::Kind::Connected;
  e.peer_id = peer;
  e.dial_key = peer;
  e.path_kind = path;
  return e;
}

LinkEvent DialFailed(const std::string& key, LinkDropReason reason, const std::string& peer_id = {}) {
  LinkEvent e;
  e.kind = LinkEvent::Kind::DialFailed;
  e.dial_key = key;
  e.peer_id = peer_id;
  e.reason = reason;
  return e;
}

LinkEvent Lost(const std::string& peer, LinkDropReason reason, uint32_t keepalive_ms) {
  LinkEvent e;
  e.kind = LinkEvent::Kind::Dropped;
  e.peer_id = peer;
  e.reason = reason;
  e.was_connected = true;
  e.keepalive_interval_ms = keepalive_ms;
  return e;
}

TEST(PeerReachRecordTest, ConnectedIsLiveWithItsPath) {
  PeerReachRecord record;
  EXPECT_EQ(record.Facts("QmSeed", kNet).state, PeerReachState::Unknown);
  record.OnLinkEvent(Connected("QmSeed", amp::LinkPathKind::Carrier), 1000, kNet);
  const PeerReachFacts f = record.Facts("QmSeed", kNet);
  EXPECT_EQ(f.state, PeerReachState::Live);
  EXPECT_EQ(f.last_ok_ms, 1000);
  EXPECT_EQ(f.last_ok_path, PeerLinkKind::Relayed);
}

TEST(PeerReachRecordTest, ADialThatGotNoAnswerIsDownAndBacksOff) {
  PeerReachRecord record;
  record.OnLinkEvent(DialFailed("QmSeed", LinkDropReason::HandshakeTimeout), 0, kNet);
  PeerReachFacts f = record.Facts("QmSeed", kNet);
  EXPECT_EQ(f.state, PeerReachState::Down);
  EXPECT_EQ(f.last_failure, PeerReachFailure::NoAnswer);
  EXPECT_EQ(f.retry_at_ms, PeerReachRecord::kRetryFirstMs);
  EXPECT_FALSE(record.MayDial("QmSeed", PeerReachRecord::kRetryFirstMs - 1, kNet));
  EXPECT_TRUE(record.MayDial("QmSeed", PeerReachRecord::kRetryFirstMs, kNet));

  for (int i = 0; i < 5; ++i) {
    record.OnLinkEvent(DialFailed("QmSeed", LinkDropReason::HandshakeTimeout), 10, kNet);
  }
  f = record.Facts("QmSeed", kNet);
  EXPECT_EQ(f.retry_at_ms, 10 + PeerReachRecord::kRetryMaxMs) << "doubling, capped";
  EXPECT_EQ(f.down_since_ms, 0) << "down since the first miss";

  record.OnLinkEvent(Connected("QmSeed"), 20, kNet);
  f = record.Facts("QmSeed", kNet);
  EXPECT_EQ(f.state, PeerReachState::Live) << "back as soon as it connects (the peer dialed us, a refresh reached it)";
  EXPECT_EQ(f.fails_in_row, 0);
}

TEST(PeerReachRecordTest, ARefusalIsAShortRetry) {
  PeerReachRecord record;
  record.OnLinkEvent(DialFailed("QmBusy", LinkDropReason::HandshakeFailed), 0, kNet);
  EXPECT_EQ(record.Facts("QmBusy", kNet).last_failure, PeerReachFailure::Refused);
  EXPECT_TRUE(record.MayDial("QmBusy", PeerReachRecord::kRetryRefusedMs, kNet));
}

TEST(PeerReachRecordTest, OnlyALossThatMeansGoneCounts) {
  PeerReachRecord record;
  record.OnLinkEvent(Lost("QmIdle", LinkDropReason::ConnectionDead, 0), 0, kNet);
  EXPECT_EQ(record.Facts("QmIdle", kNet).state, PeerReachState::Unknown) << "a cold link timing out is idle";
  record.OnLinkEvent(Lost("QmClosed", LinkDropReason::ConnectionClosed, 10000), 0, kNet);
  EXPECT_EQ(record.Facts("QmClosed", kNet).state, PeerReachState::Unknown);
  record.OnLinkEvent(Lost("QmGone", LinkDropReason::ConnectionDead, 10000), 0, kNet);
  EXPECT_EQ(record.Facts("QmGone", kNet).last_failure, PeerReachFailure::KeepaliveLost);
  record.OnLinkEvent(Lost("QmNoRoute", LinkDropReason::TransportFailed, 0), 0, kNet);
  EXPECT_EQ(record.Facts("QmNoRoute", kNet).last_failure, PeerReachFailure::Unreachable);
  record.OnLinkEvent(DialFailed("QmRace", LinkDropReason::DualDialLost), 0, kNet);
  EXPECT_EQ(record.Facts("QmRace", kNet).state, PeerReachState::Unknown) << "it answered on the other dial";
  record.OnLinkEvent(Lost("QmSilent", LinkDropReason::Unresponsive, 0), 0, kNet);
  EXPECT_EQ(record.Facts("QmSilent", kNet).last_failure, PeerReachFailure::NoAnswer) << "it did not answer a probe";
}

TEST(PeerReachRecordTest, AFailedDialNamesItsPeerOrNothing) {
  PeerReachRecord record;
  record.OnLinkEvent(DialFailed("account:abc", LinkDropReason::HandshakeTimeout, "QmBehindAlias"), 0, kNet);
  EXPECT_EQ(record.Facts("QmBehindAlias", kNet).state, PeerReachState::Down);
  record.OnLinkEvent(DialFailed("account:xyz", LinkDropReason::HandshakeTimeout), 0, kNet);
  EXPECT_EQ(record.Size(), 1u) << "an alias with no known PeerId names no peer";
}

TEST(PeerReachRecordTest, ADownMarkBelongsToItsNetwork) {
  PeerReachRecord record;
  record.OnLinkEvent(DialFailed("QmSeed", LinkDropReason::HandshakeTimeout), 0, kNet);
  EXPECT_EQ(record.Facts("QmSeed", kNet + 1).state, PeerReachState::Unknown) << "judged on the old network";
  EXPECT_TRUE(record.MayDial("QmSeed", 0, kNet + 1));
  record.OnLinkEvent(DialFailed("QmSeed", LinkDropReason::HandshakeTimeout), 5, kNet + 1);
  EXPECT_EQ(record.Facts("QmSeed", kNet + 1).fails_in_row, 1) << "a fresh count on the new network";
}

TEST(PeerReachRecordTest, ItStaysBounded) {
  PeerReachRecord record;
  for (size_t i = 0; i < PeerReachRecord::kMaxPeers + 10; ++i) {
    record.OnLinkEvent(Connected("Qm" + std::to_string(i)), static_cast<int64_t>(i), kNet);
  }
  EXPECT_EQ(record.Size(), PeerReachRecord::kMaxPeers);
  EXPECT_EQ(record.Facts("Qm0", kNet).state, PeerReachState::Unknown) << "the oldest went first";
  EXPECT_EQ(record.Facts("Qm" + std::to_string(PeerReachRecord::kMaxPeers + 9), kNet).state, PeerReachState::Live);
}

} // namespace
} // namespace pp::mesh
