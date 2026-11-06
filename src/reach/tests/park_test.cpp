#include "mesh/testing/park_harness.h"
#include "mesh/types/ServiceLoad.h"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>

namespace pp::mesh {
namespace {

using test::ParkHarness;
using RelayKind = ParkHarness::RelayKind;

std::unique_ptr<ParkHarness> MakeBench() {
  auto bench = ParkHarness::Create();
  EXPECT_TRUE(static_cast<bool>(bench)) << (bench ? "" : bench.error().message);
  return bench ? std::move(*bench) : nullptr;
}

// pp-browser #502: a phone that cannot get through to its first seed still parks on the next within a second or
// so. Dialed one after another, the unreachable seed cost its whole dial timeout (8 s) first, and a call's caller
// had walked its relays and given up (target_not_reserved) before the answerer was parked anywhere.
TEST(ParkTest, AnUnreachableFirstSeedDoesNotHoldUpTheReservation) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto dead = bench->AddRelay(RelayKind::Unreachable);
  auto live = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(dead && live);

  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*live); }, 10'000));
  EXPECT_LT(bench->ElapsedMs(), 2000) << "parked " << bench->ElapsedMs() << " ms in";
  EXPECT_FALSE(bench->ParkedOn(*dead));
}

// The park a call's answerer waits on (H010) answers as soon as a seed is Connected, past an unreachable one.
TEST(ParkTest, AParkPastAnUnreachableSeedAnswersParkedQuickly) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  ASSERT_TRUE(bench->AddRelay(RelayKind::Unreachable));
  auto live = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(live);

  std::optional<bool> parked;
  bench->Park().EnsureBootstrapSeedParkedAsync([&](bool ok) { parked = ok; }, 12'000);
  ASSERT_TRUE(bench->RunUntil([&] { return parked.has_value(); }, 15'000));
  EXPECT_TRUE(*parked);
  EXPECT_LT(bench->ElapsedMs(), 3000) << "answered " << bench->ElapsedMs() << " ms in";
  EXPECT_TRUE(bench->ParkedOn(*live));
}

// Seeds that answer: the walk stops at kRendezvousLiveTarget (2) reservations and dials no more.
TEST(ParkTest, TheWalkStopsAtTwoLiveRelays) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto first = bench->AddRelay(RelayKind::Answers);
  auto second = bench->AddRelay(RelayKind::Answers);
  auto third = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(first && second && third);

  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil([&] { return bench->Parked().size() >= 2; }, 5000));
  bench->RunUntil([] { return false; }, 2000);  // a third would have been dialed by now
  EXPECT_TRUE(bench->ParkedOn(*first));
  EXPECT_TRUE(bench->ParkedOn(*second));
  EXPECT_FALSE(bench->ParkedOn(*third));
}

// No seed answers: the park says so at its deadline, and nothing is reserved.
TEST(ParkTest, NoSeedAnswersAndTheParkSaysSoAtItsDeadline) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  ASSERT_TRUE(bench->AddRelay(RelayKind::Unreachable));
  ASSERT_TRUE(bench->AddRelay(RelayKind::Unreachable));

  std::optional<bool> parked;
  bench->Park().EnsureBootstrapSeedParkedAsync([&](bool ok) { parked = ok; }, 3000);
  ASSERT_TRUE(bench->RunUntil([&] { return parked.has_value(); }, 10'000));
  EXPECT_FALSE(*parked);
  EXPECT_GE(bench->ElapsedMs(), 3000);
  EXPECT_TRUE(bench->Parked().empty());
}

// Seeds first (H010): a relay that is no seed waits behind the seeds, so with two seeds answering it is not
// dialed at all — even listed before them.
TEST(ParkTest, SeedsAreParkedOnBeforeOtherRelays) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto other = bench->AddRelay(RelayKind::Answers, /*seed=*/false);
  auto seed_a = bench->AddRelay(RelayKind::Answers);
  auto seed_b = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(other && seed_a && seed_b);

  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil([&] { return bench->Parked().size() >= 2; }, 5000));
  bench->RunUntil([] { return false; }, 2000);
  EXPECT_TRUE(bench->ParkedOn(*seed_a));
  EXPECT_TRUE(bench->ParkedOn(*seed_b));
  EXPECT_FALSE(bench->ParkedOn(*other));
}

// The relay circuit reach chose (or the peer announced) is parked on whatever else is live: it is where the
// caller's bridge goes.
TEST(ParkTest, TheChosenRelayIsParkedOnEvenWithTwoLiveAlready) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto seed_a = bench->AddRelay(RelayKind::Answers);
  auto seed_b = bench->AddRelay(RelayKind::Answers);
  auto chosen = bench->AddRelay(RelayKind::Answers, /*seed=*/false);
  ASSERT_TRUE(seed_a && seed_b && chosen);

  bench->Park().NoteChosenRelay(*chosen);
  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*chosen); }, 5000));
  bench->RunUntil([] { return false; }, 1000);
  EXPECT_GE(bench->Parked().size(), 2u);
}

// A slow first seed (an ocean away) does not hold up a near one: the near one is parked on at once, and the slow
// one too once it answers (two live is the target).
TEST(ParkTest, ASlowFirstSeedDoesNotHoldUpANearOne) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto slow = bench->AddRelay(RelayKind::Slow, /*seed=*/true, /*rtt_ms=*/1500);
  auto near = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(slow && near);

  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*near); }, 10'000));
  EXPECT_LT(bench->ElapsedMs(), 2000) << "near parked " << bench->ElapsedMs() << " ms in";
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*slow); }, 20'000)) << "the slow seed was given up";
}

// Seeds that are only slow are no reason to give up: a park over two 500 ms seeds answers parked, on both.
TEST(ParkTest, SlowSeedsAreStillParkedOn) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto first = bench->AddRelay(RelayKind::Slow, /*seed=*/true, /*rtt_ms=*/500);
  auto second = bench->AddRelay(RelayKind::Slow, /*seed=*/true, /*rtt_ms=*/500);
  ASSERT_TRUE(first && second);

  std::optional<bool> parked;
  bench->Park().EnsureBootstrapSeedParkedAsync([&](bool ok) { parked = ok; }, 12'000);
  ASSERT_TRUE(bench->RunUntil([&] { return parked.has_value(); }, 15'000));
  EXPECT_TRUE(*parked);
  EXPECT_GE(bench->ElapsedMs(), 500) << "the bench's delay held nothing back";
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*first) && bench->ParkedOn(*second); }, 10'000));
}

// Service load (docs/PEER_REACH.md § Service load): a seed that is full for
// parking refuses the reservation and says so; the phone parks on two others
// instead of counting the full one as parked.
TEST(ParkTest, AFullSeedIsNotParkedOnAndTwoOthersAre) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto full = bench->AddRelay(RelayKind::Answers);
  auto second = bench->AddRelay(RelayKind::Answers);
  auto third = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(full && second && third);
  bench->RelayHost(*full)->OwnLoad().SetPressure(MeshService::Park, "test",
                                                 1.0);

  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil(
      [&] { return bench->ParkedOn(*second) && bench->ParkedOn(*third); },
      10'000))
      << "parked on " << bench->Parked().size();
  EXPECT_FALSE(bench->ParkedOn(*full));
  EXPECT_EQ(bench->Phone().PeerLoads().Load(*full, MeshService::Park,
                                            PeerLoadBook::NowMs()),
            ServiceLoad::Full)
      << "the refusal said why";
}

// A relay the phone is parked on starts draining: its renewal still holds, says
// draining, and the phone parks on another as well before the node goes away.
TEST(ParkTest, ADrainingRelayIsReplaced) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto first = bench->AddRelay(RelayKind::Answers);
  auto second = bench->AddRelay(RelayKind::Answers);
  auto third = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(first && second && third);
  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil(
      [&] { return bench->ParkedOn(*first) && bench->ParkedOn(*second); },
      5000));
  EXPECT_EQ(bench->Phone().PeerLoads().Load(*first, MeshService::Park,
                                            PeerLoadBook::NowMs()),
            ServiceLoad::Open)
      << "the reservation's ack says the level";

  bench->RelayHost(*first)->OwnLoad().SetDraining(true);
  bench->Park().ReserveOnBootstrapSeeds(); // the app renews every 20 s
  ASSERT_TRUE(bench->RunUntil([&] { return bench->ParkedOn(*third); }, 10'000))
      << "a replacement for the draining one";
  EXPECT_TRUE(bench->ParkedOn(*second));
  EXPECT_EQ(bench->Phone().PeerLoads().Load(*first, MeshService::Park,
                                            PeerLoadBook::NowMs()),
            ServiceLoad::Draining);
}

// A full relay still takes the renewal of a phone already parked on it: full
// means nothing new, not out.
TEST(ParkTest, AFullRelayKeepsThePhonesAlreadyParkedOnIt) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto first = bench->AddRelay(RelayKind::Answers);
  auto second = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(first && second);
  bench->Park().ReserveOnBootstrapSeeds();
  ASSERT_TRUE(bench->RunUntil(
      [&] { return bench->ParkedOn(*first) && bench->ParkedOn(*second); },
      5000));

  bench->RelayHost(*first)->OwnLoad().SetPressure(MeshService::Park, "test",
                                                  1.0);
  bench->Park().ReserveOnBootstrapSeeds();
  bench->RunUntil([] { return false; }, 3000);
  EXPECT_TRUE(bench->ParkedOn(*first)) << "renewed, not refused";
  EXPECT_EQ(bench->Phone().PeerLoads().Load(*first, MeshService::Park,
                                            PeerLoadBook::NowMs()),
            ServiceLoad::Full);
}

}  // namespace
}  // namespace pp::mesh

