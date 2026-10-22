// Choosing peers for a job from the reach record (docs/PEER_REACH.md § 5).
#include "mesh/reach/PeerReachSelect.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pp::mesh {
namespace {

constexpr int64_t kNow = 10'000'000;

ReachCandidate Unknown(const std::string& id) {
  return {id, false, {}};
}

ReachCandidate Connected(const std::string& id) {
  return {id, true, {}};
}

ReachCandidate Live(const std::string& id, const int64_t last_ok_ms) {
  ReachCandidate c{id, false, {}};
  c.facts.state = PeerReachState::Live;
  c.facts.last_ok_ms = last_ok_ms;
  return c;
}

ReachCandidate Down(const std::string& id, const int64_t retry_at_ms,
                    const PeerReachFailure failure = PeerReachFailure::NoAnswer) {
  ReachCandidate c{id, false, {}};
  c.facts.state = PeerReachState::Down;
  c.facts.retry_at_ms = retry_at_ms;
  c.facts.last_failure = failure;
  return c;
}

using Ids = std::vector<std::string>;

TEST(PeerReachSelectTest, LiveFirstLatestSuccessFirstThenTheRestInPolicyOrder) {
  const auto pick = SelectForRole({Unknown("u1"), Live("old", 100), Unknown("u2"), Live("new", 900)}, kNow,
                                  /*live_target=*/2, /*max_cold=*/4);
  EXPECT_EQ(pick.cold, (Ids{"new", "old", "u1", "u2"}));
  EXPECT_TRUE(pick.connected.empty());
  EXPECT_TRUE(pick.down.empty());
}

TEST(PeerReachSelectTest, APeerHeldDownIsNotDialedUntilItsRetryTime) {
  const auto held = SelectForRole({Down("gone", kNow + 1), Unknown("u")}, kNow, 2, 3);
  EXPECT_EQ(held.cold, (Ids{"u"}));
  EXPECT_EQ(held.down, (Ids{"gone"}));
  const auto due = SelectForRole({Down("gone", kNow), Unknown("u")}, kNow, 2, 3);
  EXPECT_EQ(due.cold, (Ids{"gone", "u"})) << "past its retry time it is tried again, in policy order";
  EXPECT_TRUE(due.down.empty());
}

TEST(PeerReachSelectTest, ColdDialsAreCappedAndNoneOnceConnectedMeetTheTarget) {
  const auto capped = SelectForRole({Unknown("a"), Unknown("b"), Unknown("c"), Unknown("d")}, kNow,
                                    kRendezvousLiveTarget, kReachMaxColdDialsPerRound);
  EXPECT_EQ(capped.cold, (Ids{"a", "b", "c"}));
  const auto met = SelectForRole({Connected("a"), Unknown("b"), Connected("c")}, kNow, 2, 3);
  EXPECT_EQ(met.connected, (Ids{"a", "c"}));
  EXPECT_TRUE(met.cold.empty());
}

TEST(PeerReachSelectTest, AConnectedPeerCountsWhateverTheRecordSays) {
  ReachCandidate c = Down("x", kNow + 60'000);
  c.connected = true;
  const auto pick = SelectForRole({c}, kNow, 2, 3);
  EXPECT_EQ(pick.connected, (Ids{"x"}));
  EXPECT_TRUE(pick.down.empty());
}

TEST(PeerReachSelectTest, TheMustDialRelayLeadsUnlessHeldDown) {
  const auto pick = SelectForRole({Connected("a"), Connected("b"), Live("r", 5), Unknown("sticky")}, kNow, 2, 3, "sticky");
  EXPECT_EQ(pick.cold, (Ids{"sticky"})) << "dialed although the target is met";
  const auto first = SelectForRole({Live("r", 5), Unknown("sticky")}, kNow, 2, 3, "sticky");
  EXPECT_EQ(first.cold, (Ids{"sticky", "r"}));
  const auto held = SelectForRole({Down("sticky", kNow + 1), Unknown("u")}, kNow, 2, 3, "sticky");
  EXPECT_EQ(held.cold, (Ids{"u"}));
  EXPECT_EQ(held.down, (Ids{"sticky"}));
}

TEST(PeerReachSelectTest, AParkNeedsTheTargetOrEveryUsableRelay) {
  EXPECT_EQ(SeedParkNeeds(5), kRendezvousLiveTarget);
  EXPECT_EQ(SeedParkNeeds(1), 1u);
  EXPECT_EQ(SeedParkNeeds(0), 0u) << "every relay held down: nothing to wait for";
}

TEST(PeerReachSelectTest, OnlyAMissThatMayPassIsRedialed) {
  EXPECT_TRUE(SeedMissMayPass({}, kNow)) << "the record has nothing against it";
  EXPECT_FALSE(SeedMissMayPass(Down("s", kNow + 300'000).facts, kNow)) << "no answer to a whole dial";
  EXPECT_TRUE(SeedMissMayPass(Down("s", kNow + 60'000, PeerReachFailure::Refused).facts, kNow)) << "up, busy";
}

// A job the app drives (directory, DHT): up first, then latest successes, never a peer held down.
TEST(PeerReachSelectTest, AJobGetsUpPeersThenRecentSuccessesWithinItsCap) {
  const auto picked = ChooseForJob({Unknown("u1"), Down("gone", kNow + 1), Live("old", 1), Connected("up"),
                                    Live("new", 9), Unknown("u2")},
                                   kNow, 4);
  EXPECT_EQ(picked, (Ids{"up", "new", "old", "u1"}));
  EXPECT_TRUE(ChooseForJob({Down("gone", kNow + 1)}, kNow, 3).empty()) << "nothing worth asking";
}

} // namespace
} // namespace pp::mesh
