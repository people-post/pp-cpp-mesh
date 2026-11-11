// The dial side of the rendezvous on a bench (mesh::test::ParkHarness): a caller's circuit reach bridges to a parked
// phone through relays that answer or never do, two at a time (docs/PEER_REACH.md § 5, Circuit dial).
#include "mesh/l4/circuit/CircuitHopAttemptBudget.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/testing/park_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

namespace pp::mesh {
namespace {

using test::ParkHarness;
using RelayKind = ParkHarness::RelayKind;

std::unique_ptr<ParkHarness> MakeBench() {
  auto bench = ParkHarness::Create();
  EXPECT_TRUE(static_cast<bool>(bench)) << (bench ? "" : bench.error().message);
  if (!bench) {
    return nullptr;
  }
  auto caller = (*bench)->AddCaller();
  EXPECT_TRUE(static_cast<bool>(caller)) << (caller ? "" : caller.error().message);
  return caller ? std::move(*bench) : nullptr;
}

/** Parks the phone on `live` (the live relays), then runs one circuit from the caller; how long it took. */
struct CircuitRun {
  std::optional<CircuitReachRoe> result;
  int64_t took_ms = -1;
};

CircuitRun ParkThenReach(ParkHarness& bench, const std::vector<std::string>& live) {
  bench.Park().ReserveOnBootstrapSeeds();
  EXPECT_TRUE(bench.RunUntil(
      [&] {
        return std::all_of(live.begin(), live.end(), [&](const std::string& relay) { return bench.ParkedOn(relay); });
      },
      15'000))
      << "the phone did not park";
  CircuitRun run;
  const int64_t start = bench.ElapsedMs();
  bench.CallerReach().TryEnsureCircuitOnlyAsync(bench.PhonePeerId(), [&run](CircuitReachRoe r) { run.result = r; },
                                                {});
  bench.RunUntil([&] { return run.result.has_value(); }, 30'000);
  run.took_ms = bench.ElapsedMs() - start;
  return run;
}

size_t BridgesOn(ParkHarness& bench, const std::string& relay) {
  MeshHost* host = bench.RelayHost(relay);
  return host && host->AmpCircuitServer() ? host->AmpCircuitServer()->RuntimeStats().active_bridges : 0;
}

size_t PendingOn(ParkHarness& bench, const std::string& relay) {
  MeshHost* host = bench.RelayHost(relay);
  return host && host->AmpCircuitServer() ? host->AmpCircuitServer()->RuntimeStats().pending_tunnels : 0;
}

// pp-browser #577: the first relay tried never answered, and the circuit waited its whole bridge timeout (4 s)
// before trying the second. Bridged to together, the second's answer is the circuit's.
TEST(CircuitWalkTest, ASilentFirstRelayCostsNoBridgeTimeout) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto silent = bench->AddRelay(RelayKind::Unreachable);
  auto live = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(silent && live);

  const CircuitRun run = ParkThenReach(*bench, {*live});
  ASSERT_TRUE(run.result.has_value()) << "no answer";
  ASSERT_TRUE(*run.result) << run.result->error().message;
  EXPECT_LT(run.took_ms, 2000) << "the silent relay held the circuit up";
  EXPECT_EQ(bench->CallerReach().LastGoodRelayPeerKey(), *live);
  EXPECT_TRUE(bench->Caller().ChatDeps()->links.IsConnected(bench->PhonePeerId()));
}

// Two relays that both answer: one circuit, through the first to ack; the other bridge is cancelled, and nothing
// of it stays at its relay or at the caller.
TEST(CircuitWalkTest, TwoLiveRelaysMakeOneCircuitAndTheOtherIsCancelled) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto first = bench->AddRelay(RelayKind::Answers);
  auto second = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(first && second);

  const CircuitRun run = ParkThenReach(*bench, {*first, *second});
  ASSERT_TRUE(run.result.has_value()) << "no answer";
  ASSERT_TRUE(*run.result) << run.result->error().message;
  bench->RunUntil([] { return false; }, 3000); // the loser's teardown crosses the hub
  const std::string winner = bench->CallerReach().LastGoodRelayPeerKey();
  ASSERT_TRUE(winner == *first || winner == *second) << winner;
  const std::string loser = winner == *first ? *second : *first;
  EXPECT_EQ(BridgesOn(*bench, winner), 1u);
  EXPECT_EQ(BridgesOn(*bench, loser), 0u) << "the cancelled bridge still spliced at its relay";
  EXPECT_EQ(PendingOn(*bench, loser), 0u);
  EXPECT_EQ(bench->Caller().AmpCircuitClient()->TunnelCount(), 1u) << "the caller holds the winner only";
  EXPECT_TRUE(bench->Caller().ChatDeps()->links.IsConnected(bench->PhonePeerId()));
}

// Both relays bridged to first never answer: the walk goes on down the list, as one at a time did — the pair cost
// one bridge timeout, not two.
TEST(CircuitWalkTest, TwoSilentRelaysFallThroughToTheNext) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  auto silent_a = bench->AddRelay(RelayKind::Unreachable);
  auto silent_b = bench->AddRelay(RelayKind::Unreachable);
  auto live = bench->AddRelay(RelayKind::Answers);
  ASSERT_TRUE(silent_a && silent_b && live);

  const CircuitRun run = ParkThenReach(*bench, {*live});
  ASSERT_TRUE(run.result.has_value()) << "no answer";
  ASSERT_TRUE(*run.result) << run.result->error().message;
  EXPECT_GE(run.took_ms, 4000) << "the silent pair's bridge timeout first";
  EXPECT_LT(run.took_ms, 7000);
  EXPECT_EQ(bench->CallerReach().LastGoodRelayPeerKey(), *live);
}

// No relay answers: the walk gives up within its envelope, having spent its bridges, and holds nothing.
TEST(CircuitWalkTest, NoRelayAnswersAndTheWalkEndsInItsEnvelope) {
  auto bench = MakeBench();
  ASSERT_NE(bench, nullptr);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(bench->AddRelay(RelayKind::Unreachable));
  }
  const CircuitRun run = ParkThenReach(*bench, {});
  ASSERT_TRUE(run.result.has_value()) << "no answer";
  EXPECT_FALSE(*run.result);
  EXPECT_LE(run.took_ms, kCircuitReachEnvelopeMs + 1000);
  bench->RunUntil([] { return false; }, 1000);
  EXPECT_EQ(bench->Caller().AmpCircuitClient()->TunnelCount(), 0u);
}

} // namespace
} // namespace pp::mesh
