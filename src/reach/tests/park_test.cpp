#include "mesh/testing/park_harness.h"

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

}  // namespace
}  // namespace pp::mesh

