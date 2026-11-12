// PeerReachRequest::race_circuit on a bench: the reach, its dial and its circuit on one ManualExecutor lane, so
// every timing below is virtual milliseconds (docs/PEER_REACH.md § 5, PeerReach).
#include "mesh/reach/PeerReachCoordinator.h"

#include "mesh/reach/MeshReachPorts.h"
#include "common/ManualExecutor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pp::mesh {
namespace {

constexpr const char* kPeer = "12D3KooWRacePeer";
constexpr const char* kPrivateMa = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWRacePeer";
constexpr const char* kPublicMa = "/ip4/203.0.113.7/udp/1/p2p/12D3KooWRacePeer";

using Ms = std::chrono::milliseconds;

/** A dial book whose dials answer `dial_ms` later on the lane: connected, or a handshake timeout. */
class TimedDialRegistry final : public IDialRegistry {
public:
  explicit TimedDialRegistry(ManualExecutor& lane) : lane_(lane) {}

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }
  bool IsDialable(const std::string& peer_key) const override { return endpoints.count(peer_key) > 0; }
  bool IsConnected(const std::string& peer_key) const override { return connected.count(peer_key) > 0; }
  bool IsConnectedDirect(const std::string& peer_key) const override {
    return connected.count(peer_key) > 0 && carrier_only.count(peer_key) == 0;
  }
  void EnsureAssociation(const std::string& peer_key, IChatPeerLinks::LinkCb on_done) override {
    ++dials;
    ++dials_out;
    (void)lane_.PostAfter(Ms(dial_ms), [this, peer_key, on_done = std::move(on_done)]() {
      --dials_out;
      if (dial_connects) {
        connected.insert(peer_key);
        carrier_only.erase(peer_key); // the direct link is the peer's link now
        on_done({});
        return;
      }
      on_done(IChatPeerLinks::Failure::Of(IChatPeerLinks::Err::DialTimeout, "amp link: handshake-timeout"));
    });
  }
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (auto it = endpoints.find(peer_key); it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  void ClearDialBackoff(const std::string& /*peer_key*/) override {}
  void AbortInflightDial(const std::string& /*peer_key*/) override {}
  void ClearPeerCircuitHop(const std::string& peer_key) override { hops.erase(peer_key); }
  bool HasPeerCircuitHop(const std::string& peer_key) const override { return hops.count(peer_key) > 0; }

  /** The circuit landed: a relay carrier link (and its hop), unless a direct link is up already. */
  void ConnectRelayed(const std::string& key) {
    hops.insert(key);
    if (connected.insert(key).second) {
      carrier_only.insert(key);
    }
  }

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_set<std::string> connected;
  std::unordered_set<std::string> carrier_only;
  std::unordered_set<std::string> hops;
  int64_t dial_ms = 300;
  bool dial_connects = false;
  int dials = 0;
  int dials_out = 0;

private:
  ManualExecutor& lane_;
};

/** Circuit reach whose circuit answers `circuit_ms` later, and whose punch `punch_ms` later (never connecting). */
class TimedCircuitReach final : public ICircuitHopReach {
public:
  TimedCircuitReach(ManualExecutor& lane, TimedDialRegistry& dial) : lane_(lane), dial_(dial) {}

  CircuitReachRoe TryEnsureHopReachable(const std::string& /*hop*/) override { return {}; }
  CircuitReachRoe TryEnsurePeerReachable(const std::string& /*peer*/) override {
    return CircuitReachFailure::Of(CircuitReachErr::Generic, "sync not used");
  }
  void TryEnsurePeerReachableViaAsync(const std::string& peer_key, CircuitReachDone on_done, const bool allow_circuit,
                                      std::vector<std::string> /*advertised_relays*/) override {
    if (allow_circuit) {
      ++circuit_then_punch;
      Circuit(peer_key, std::move(on_done));
      return;
    }
    ++punches;
    punch_at = lane_.Now().count();
    dials_out_at_punch = dial_.dials_out;
    (void)lane_.PostAfter(Ms(punch_ms), [on_done = std::move(on_done)]() {
      on_done(CircuitReachFailure::Of(CircuitReachErr::PunchFailed, "punch failed"));
    });
  }
  void TryEnsureCircuitOnlyAsync(const std::string& peer_key, CircuitReachDone on_done,
                                 std::vector<std::string> /*advertised_relays*/) override {
    ++circuit_only;
    circuit_only_at = lane_.Now().count();
    Circuit(peer_key, std::move(on_done));
  }

  int64_t circuit_ms = 3000;
  bool circuit_connects = true;
  int64_t punch_ms = 1000;
  int circuit_then_punch = 0;
  int circuit_only = 0;
  int punches = 0;
  int64_t circuit_only_at = -1;
  int64_t punch_at = -1;
  int dials_out_at_punch = -1;

private:
  void Circuit(const std::string& peer_key, CircuitReachDone on_done) {
    (void)lane_.PostAfter(Ms(circuit_ms), [this, peer_key, on_done = std::move(on_done)]() {
      if (circuit_connects) {
        dial_.ConnectRelayed(peer_key);
        on_done({});
        return;
      }
      on_done(CircuitReachFailure::Of(CircuitReachErr::TargetNotReserved, "circuit hop reach failed: not registered"));
    });
  }

  ManualExecutor& lane_;
  TimedDialRegistry& dial_;
};

class PeerReachRaceTest : public ::testing::Test {
protected:
  void SetUp() override {
    lane_ = std::make_shared<ManualExecutor>();
    dial_ = std::make_unique<TimedDialRegistry>(*lane_);
    circuit_ = std::make_unique<TimedCircuitReach>(*lane_, *dial_);
    PeerReachPorts ports{dial_.get(), circuit_.get(), lane_};
    ports.now_ms = [lane = lane_.get()]() { return lane->Now().count(); };
    reach_ = std::make_unique<PeerReachCoordinator>(std::move(ports)); // the production dial budget (12 s)
  }

  void TearDown() override {
    reach_->CancelAll();
    reach_.reset();
    // A stopping runtime drops its lane's tasks; this lane is the test's, so run them out. A pending dial or
    // circuit completion captures the lane, and left queued it kept the lane alive (LeakSanitizer).
    for (int i = 0; i < 1000 && !lane_->Idle(); ++i) {
      lane_->AdvanceBy(Ms(1000));
    }
  }

  struct Outcome {
    std::optional<PeerReachCoordinator::ReachRoe> result;
    int64_t at_ms = -1;
  };

  /** Runs one reach to its end on virtual time (at most `max_ms`). */
  Outcome Run(PeerReachRequest req, const int64_t max_ms = 60'000) {
    Outcome out;
    reach_->Ensure(std::move(req), [&out, lane = lane_.get()](PeerReachCoordinator::ReachRoe r) {
      out.result = std::move(r);
      out.at_ms = lane->Now().count();
    });
    const int64_t until = lane_->Now().count() + max_ms;
    while (!out.result && lane_->Now().count() < until) {
      lane_->AdvanceBy(Ms(10));
    }
    return out;
  }

  static PeerReachRequest Request(const bool race, PeerReachMode mode = PeerReachMode::Reach) {
    PeerReachRequest r;
    r.keys = {kPeer};
    r.mode = mode;
    r.race_circuit = race;
    return r;
  }

  /** A peer at a public address the dial cannot reach: every handshake times out (#577: ~9 s). */
  void UndialablePeer() {
    dial_->endpoints[kPeer] = kPublicMa;
    dial_->dial_ms = 8900;
    dial_->dial_connects = false;
  }

  std::shared_ptr<ManualExecutor> lane_; // first: outlives what posts to it
  std::unique_ptr<TimedDialRegistry> dial_;
  std::unique_ptr<TimedCircuitReach> circuit_;
  std::unique_ptr<PeerReachCoordinator> reach_;
};

// pp-browser #577: the dial timed out at ~9 s, the pivot waited for the 12 s budget, then the circuit. Raced, the
// circuit's own time is the reach's.
TEST_F(PeerReachRaceTest, AnUndialablePeerIsReachedInTheCircuitsTime) {
  UndialablePeer();
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Relayed);
  EXPECT_EQ(circuit_->circuit_only_at, 0) << "the circuit starts with the dial";
  EXPECT_LT(out.at_ms, 3500) << "the circuit takes 3 s";
  EXPECT_EQ(dial_->dials, 1);
}

// Without the option nothing changes: the dial budget first, then the circuit (and its punch).
TEST_F(PeerReachRaceTest, WithoutTheRaceTheCircuitWaitsForTheDialBudget) {
  UndialablePeer();
  Outcome out = Run(Request(/*race=*/false));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Relayed);
  EXPECT_GE(out.at_ms, 12'000 + 3000);
  EXPECT_LT(out.at_ms, 12'000 + 3500);
  EXPECT_EQ(circuit_->circuit_only, 0);
  EXPECT_EQ(circuit_->circuit_then_punch, 1);
}

// A peer the dial reaches still ends on the direct link when that lands first; the circuit built beside it
// changes nothing about the answer.
TEST_F(PeerReachRaceTest, ADirectLinkThatLandsFirstWins) {
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->dial_ms = 300;
  dial_->dial_connects = true;
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Direct);
  EXPECT_EQ(out.at_ms, 300);
  EXPECT_EQ(circuit_->circuit_only, 1) << "the circuit was raced";
}

// The other way round: the circuit lands first and settles it, the dial still out.
TEST_F(PeerReachRaceTest, ACircuitThatLandsFirstWins) {
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->dial_ms = 5000;
  dial_->dial_connects = true;
  circuit_->circuit_ms = 1000;
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Relayed);
  EXPECT_EQ(out.at_ms, 1000);
}

// The deadline covers the circuit from its start: one slower than the dial budget still lands.
TEST_F(PeerReachRaceTest, ARacedCircuitSlowerThanTheDialBudgetStillLands) {
  UndialablePeer();
  dial_->dial_ms = 500;
  circuit_->circuit_ms = 14'000; // the circuit's whole envelope
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Relayed);
  EXPECT_EQ(out.at_ms, 14'000);
}

// The raced circuit misses: the dial keeps redialing within its budget, and the punch (the last resort) waits
// until the dial is done — never beside it. The failure is the circuit's.
TEST_F(PeerReachRaceTest, ThePunchWaitsUntilTheDialIsDone) {
  UndialablePeer();
  dial_->dial_ms = 500;
  circuit_->circuit_ms = 1000;
  circuit_->circuit_connects = false;
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result);
  EXPECT_FALSE(*out.result);
  EXPECT_EQ(out.result->error().GetCode(), PeerReachCoordinator::Err::CircuitFailed) << out.result->error().message;
  EXPECT_GE(dial_->dials, 3) << "redials beside the circuit";
  EXPECT_EQ(circuit_->punches, 1);
  EXPECT_GE(circuit_->punch_at, 10'500) << "the dial's budget first";
  EXPECT_EQ(circuit_->dials_out_at_punch, 0);
  EXPECT_EQ(circuit_->circuit_then_punch, 0) << "no second circuit";
}

// A private Preferred is parked for first and skipped once parked (H010), race or not: the circuit then starts at
// once anyway, as without the race.
TEST_F(PeerReachRaceTest, APrivatePreferredIsStillParkedForFirst) {
  int parks = 0;
  reach_->SetSeedParkAwait([&parks](std::function<void(bool)> done, int /*timeout_ms*/) {
    ++parks;
    done(true);
  });
  dial_->endpoints[kPeer] = kPrivateMa;
  Outcome out = Run(Request(/*race=*/true));
  ASSERT_TRUE(out.result && *out.result) << (out.result ? out.result->error().message : "no answer");
  EXPECT_EQ((*out.result)->kind, PeerLinkKind::Relayed);
  EXPECT_GE(parks, 1);
  EXPECT_EQ(dial_->dials, 0) << "parked: the private Preferred is skipped";
  EXPECT_EQ(circuit_->circuit_only, 0);
  EXPECT_EQ(circuit_->circuit_then_punch, 1);
  EXPECT_LT(out.at_ms, 3500);
}

// Await builds no circuit of its own: the option changes nothing there.
TEST_F(PeerReachRaceTest, AwaitIgnoresTheRace) {
  dial_->endpoints[kPeer] = kPrivateMa;
  (void)Run(Request(/*race=*/true, PeerReachMode::Await));
  EXPECT_EQ(circuit_->circuit_only, 0);
  EXPECT_EQ(circuit_->circuit_then_punch, 0);
  EXPECT_EQ(circuit_->punches, 1);
  EXPECT_EQ(dial_->dials, 0);
}

} // namespace
} // namespace pp::mesh
