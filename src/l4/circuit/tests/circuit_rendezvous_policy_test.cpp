#include "mesh/l4/circuit/CircuitRendezvousPolicy.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

TEST(CircuitRendezvousPolicyTest, ParkOrderConnectedFirstStable) {
  const std::vector<std::string> in = {"a", "b", "c", "d"};
  const std::unordered_set<std::string> connected = {"c", "a"};
  const auto out = pp::mesh::OrderRendezvousParkAttempts(in, [&](const std::string& key) {
    return connected.count(key) > 0;
  });
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "c");
  EXPECT_EQ(out[2], "b");
  EXPECT_EQ(out[3], "d");
}

TEST(CircuitRendezvousPolicyTest, ParkOrderStickyThenConnected) {
  const std::vector<std::string> in = {"a", "b", "c", "d"};
  const std::unordered_set<std::string> connected = {"c"};
  const auto out = pp::mesh::OrderRendezvousParkAttempts(in, "b", [&](const std::string& key) {
    return connected.count(key) > 0;
  });
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], "b");
  EXPECT_EQ(out[1], "c");
  EXPECT_EQ(out[2], "a");
  EXPECT_EQ(out[3], "d");
}

TEST(CircuitRendezvousPolicyTest, ParkOrderWithoutConnectedKeepsInputOrder) {
  const std::vector<std::string> in = {"a", "b", "c"};
  const auto out = pp::mesh::OrderRendezvousParkAttempts(in, [](const std::string&) { return false; });
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "b");
  EXPECT_EQ(out[2], "c");
}

// call-path-resilience k2: one unreachable seed used to hold the answerer's Accept for the whole
// 12 s park deadline. Now ≥1 Connected arms a short grace; enough Connected settles at once.
TEST(SeedParkStepTest, OneSeedConnectedArmsGraceInsteadOfWaitingTheDeadline) {
  using pp::mesh::DecideSeedParkStep;
  using pp::mesh::SeedParkStep;
  EXPECT_EQ(DecideSeedParkStep(/*enough=*/false, /*any=*/true, /*at_deadline=*/false, /*grace_armed=*/false),
            SeedParkStep::ArmGrace);
  EXPECT_EQ(DecideSeedParkStep(false, true, false, /*grace_armed=*/true), SeedParkStep::Wait) << "grace armed once";
  EXPECT_EQ(DecideSeedParkStep(false, true, /*at_deadline (grace end)=*/true, true), SeedParkStep::SettleOk);
}

TEST(SeedParkStepTest, EnoughConnectedSettlesAndNoneWaitsUntilTheDeadline) {
  using pp::mesh::DecideSeedParkStep;
  using pp::mesh::SeedParkStep;
  EXPECT_EQ(DecideSeedParkStep(true, true, false, false), SeedParkStep::SettleOk);
  EXPECT_EQ(DecideSeedParkStep(false, false, false, false), SeedParkStep::Wait);
  EXPECT_EQ(DecideSeedParkStep(false, false, true, false), SeedParkStep::SettleFail);
}

// A seed dial lost while a park waits is dialed again — while enough of the deadline is left for a
// handshake, at most kSeedParkMaxRedials times (one lost dial used to cost the whole 12 s).
TEST(CircuitRendezvousPolicy, SeedParkRedialsWhileTimeIsLeft) {
  using pp::mesh::kSeedParkMaxRedials;
  using pp::mesh::kSeedParkRedialMinLeftMs;
  using pp::mesh::ShouldRedialSeedForPark;
  EXPECT_TRUE(ShouldRedialSeedForPark(8000, 0));
  EXPECT_TRUE(ShouldRedialSeedForPark(kSeedParkRedialMinLeftMs, kSeedParkMaxRedials - 1));
  EXPECT_FALSE(ShouldRedialSeedForPark(kSeedParkRedialMinLeftMs - 1, 0)) << "too little left for a handshake";
  EXPECT_FALSE(ShouldRedialSeedForPark(8000, kSeedParkMaxRedials)) << "redials used up";
}

// With no seed Connected, none still dialing and no redial left, the park fails now: seeds that
// refuse or are unreachable no longer hold the answer for the whole deadline.
TEST(CircuitRendezvousPolicy, SeedParkFailsEarlyOnlyWhenNothingCanLand) {
  using pp::mesh::SeedParkCannotLand;
  EXPECT_TRUE(SeedParkCannotLand(/*any_seed_connected=*/false, /*seed_dials_in_flight=*/0, /*redial_left=*/false));
  EXPECT_FALSE(SeedParkCannotLand(true, 0, false)) << "a Connected seed parks it";
  EXPECT_FALSE(SeedParkCannotLand(false, 1, false)) << "another seed's dial may still land";
  EXPECT_FALSE(SeedParkCannotLand(false, 0, true)) << "a redial is still worth trying";
}
