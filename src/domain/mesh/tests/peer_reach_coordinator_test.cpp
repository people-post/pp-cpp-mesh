#include "domain/mesh/reach/PeerReachCoordinator.h"

#include "domain/mesh/reach/MeshReachPorts.h"
#include "foundation/runtime/AppRuntime.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pbr {
namespace {

constexpr const char* kPeer = "12D3KooWReachPeer";
constexpr const char* kPrivateMa = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWReachPeer";
constexpr const char* kPublicMa = "/ip4/203.0.113.7/udp/1/p2p/12D3KooWReachPeer";

/** Dial registry fake; state is read on the Connectivity owner, so every access takes the lock. */
class FakeDialRegistry final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    std::lock_guard lock(mu);
    endpoints[peer_key] = multiaddr;
    return {};
  }
  bool IsDialable(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    return endpoints.count(peer_key) > 0;
  }
  bool IsConnected(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    return connected.count(peer_key) > 0;
  }
  bool IsConnectedDirect(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    return connected.count(peer_key) > 0 && carrier_only.count(peer_key) == 0;
  }
  void EnsureAssociation(const std::string& peer_key, std::function<void(Roe<void>)> on_done) override {
    ensure_calls.fetch_add(1);
    bool ok = false;
    {
      std::lock_guard lock(mu);
      ok = ensure_connects;
      if (ok) {
        connected.insert(peer_key);
      }
    }
    if (const int delay = miss_delay_ms.load(); !ok && delay > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));  // a slow miss
    }
    on_done(ok ? Roe<void>() : Roe<void>(Error("amp link: dial in backoff")));
  }
  /** A failing dial reports its miss this late (the coordinator's budget may be spent by then). */
  std::atomic<int> miss_delay_ms{0};
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    if (auto it = endpoints.find(peer_key); it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  void ClearDialBackoff(const std::string& /*peer_key*/) override { clear_backoff_calls.fetch_add(1); }
  void AbortInflightDial(const std::string& /*peer_key*/) override { abort_calls.fetch_add(1); }
  void DropLink(const std::string& peer_key) override {
    drop_calls.fetch_add(1);
    std::lock_guard lock(mu);
    connected.erase(peer_key);
  }
  void ClearPeerCircuitHop(const std::string& peer_key) override {
    std::lock_guard lock(mu);
    circuit_hops.erase(peer_key);
  }
  bool HasPeerCircuitHop(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    return circuit_hops.count(peer_key) > 0;
  }

  void Connect(const std::string& key, bool carrier = false, bool hop = false) {
    std::lock_guard lock(mu);
    connected.insert(key);
    if (carrier) {
      carrier_only.insert(key);
    }
    if (hop) {
      circuit_hops.insert(key);
    }
  }

  mutable std::mutex mu;
  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_set<std::string> connected;
  std::unordered_set<std::string> carrier_only;
  std::unordered_set<std::string> circuit_hops;
  bool ensure_connects = false;
  std::atomic<int> ensure_calls{0};
  std::atomic<int> clear_backoff_calls{0};
  std::atomic<int> drop_calls{0};
  std::atomic<int> abort_calls{0};
};

class FakeCircuitReach final : public ICircuitHopReach {
public:
  explicit FakeCircuitReach(FakeDialRegistry& dial) : dial_(dial) {}

  Roe<void> TryEnsureHopReachable(const std::string& /*hop*/) override { return {}; }
  Roe<void> TryEnsurePeerReachable(const std::string& /*peer*/) override {
    return Error("sync not used");
  }
  void TryEnsurePeerReachableAsync(const std::string& peer_key, std::function<void(Roe<void>)> on_done,
                                        const bool allow_circuit) override {
    calls.fetch_add(1);
    last_allow_circuit.store(allow_circuit);
    if (connects) {
      // A circuit (allowed) lands a relayed link; punch-only lands a direct one.
      dial_.Connect(peer_key, /*carrier=*/allow_circuit, /*hop=*/allow_circuit);
      on_done({});
      return;
    }
    on_done(Error("circuit hop reach failed"));
  }

  std::atomic<int> calls{0};
  std::atomic<bool> last_allow_circuit{false};
  bool connects = true;

private:
  FakeDialRegistry& dial_;
};

class PeerReachCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dial_ = std::make_unique<FakeDialRegistry>();
    circuit_ = std::make_unique<FakeCircuitReach>(*dial_);
    reach_ = std::make_unique<PeerReachCoordinator>(dial_.get(), circuit_.get());
    reach_->SetDialBudgetMsForTest(300);
  }

  void TearDown() override {
    reach_->CancelAll();
    AppRuntime::Shutdown();
    reach_.reset();
    circuit_.reset();
    dial_.reset();
  }

  struct Outcome {
    std::mutex mu;
    std::atomic<bool> done{false};
    std::optional<Roe<PeerReachResult>> result;
  };

  std::shared_ptr<Outcome> Run(PeerReachRequest req) {
    auto out = std::make_shared<Outcome>();
    last_id_ = reach_->Ensure(std::move(req), [out](Roe<PeerReachResult> r) {
      std::lock_guard lock(out->mu);
      out->result = std::move(r);
      out->done.store(true, std::memory_order_release);
    });
    return out;
  }

  static bool WaitDone(const std::shared_ptr<Outcome>& out, std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!out->done.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= until) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  static PeerReachRequest Request(PeerReachMode mode = PeerReachMode::Reach) {
    PeerReachRequest r;
    r.keys = {kPeer};
    r.mode = mode;
    return r;
  }

  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCircuitReach> circuit_;
  std::unique_ptr<PeerReachCoordinator> reach_;
  PeerReachId last_id_ = 0;
};

TEST_F(PeerReachCoordinatorTest, ConnectedLinkIsReusedWithoutDial) {
  dial_->Connect(kPeer);
  auto out = Run(Request());
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(5)));
  ASSERT_TRUE(*out->result);
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Direct);
  EXPECT_TRUE((*out->result)->reused_link);
  EXPECT_EQ(dial_->ensure_calls.load(), 0);
  EXPECT_EQ(circuit_->calls.load(), 0);
}

// Dogfood 2026-09-24: a relay-carrier-only link reads "connected" but is not direct.
TEST_F(PeerReachCoordinatorTest, CarrierOnlyLinkIsRelayed) {
  dial_->Connect(kPeer, /*carrier=*/true);
  auto out = Run(Request());
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(5)));
  ASSERT_TRUE(*out->result);
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
}

// B39: a stale "connected" link must be redialed, not reused.
TEST_F(PeerReachCoordinatorTest, FreshLinkRedialsInsteadOfReusing) {
  dial_->Connect(kPeer);
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->ensure_connects = true;
  auto req = Request();
  req.fresh_link = true;
  auto out = Run(req);
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(5)));
  ASSERT_TRUE(*out->result);
  EXPECT_FALSE((*out->result)->reused_link);
  EXPECT_EQ(dial_->drop_calls.load(), 1) << "stale link dropped before redial";
  EXPECT_GE(dial_->ensure_calls.load(), 1);
}

TEST_F(PeerReachCoordinatorTest, PublicPreferredDialsDirect) {
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->ensure_connects = true;
  auto out = Run(Request());
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(5)));
  ASSERT_TRUE(*out->result);
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Direct);
  EXPECT_EQ(circuit_->calls.load(), 0);
}

// B31 / V049: a dial miss re-dials within budget, then pivots to a circuit (Reach builds one).
TEST_F(PeerReachCoordinatorTest, ReachPivotsToCircuitAfterDialBudget) {
  dial_->endpoints[kPeer] = kPublicMa;
  auto out = Run(Request(PeerReachMode::Reach));
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(10)));
  ASSERT_TRUE(*out->result) << out->result->error().message;
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
  EXPECT_GE(dial_->ensure_calls.load(), 1);
  EXPECT_GE(dial_->clear_backoff_calls.load(), 1) << "a dial miss clears backoff for the next try";
  EXPECT_TRUE(circuit_->last_allow_circuit.load());
}

// macOS CI: the dial's last miss landed with the budget spent, and the poll that should pivot ran
// past the deadline — which failed the reach without ever trying the circuit. A spent budget with
// no circuit tried yet pivots, however late the poll runs.
TEST_F(PeerReachCoordinatorTest, MissAfterTheBudgetStillPivotsToCircuit) {
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->miss_delay_ms = 400;  // past the 300 ms test budget
  auto out = Run(Request(PeerReachMode::Reach));
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(10)));
  ASSERT_TRUE(*out->result) << out->result->error().message;
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
  EXPECT_EQ(circuit_->calls.load(), 1);
}

// Dogfood ae4900eb / 39412f / 072a7425: the awaiting side never dials a private Preferred and
// never builds its own circuit — it punches and waits for the reacher.
TEST_F(PeerReachCoordinatorTest, AwaitSkipsPrivateDialAndPunchesOnly) {
  dial_->endpoints[kPeer] = kPrivateMa;
  auto out = Run(Request(PeerReachMode::Await));
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(10)));
  ASSERT_TRUE(*out->result) << out->result->error().message;
  EXPECT_EQ(dial_->ensure_calls.load(), 0);
  EXPECT_EQ(circuit_->calls.load(), 1);
  EXPECT_FALSE(circuit_->last_allow_circuit.load());
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Punched);
}

// k6: a mobile pair's awaiting side does not punch — it waits for the peer's circuit to land.
TEST_F(PeerReachCoordinatorTest, AwaitWithoutPunchWaitsForThePeersCircuit) {
  dial_->endpoints[kPeer] = kPrivateMa;
  auto req = Request(PeerReachMode::Await);
  req.allow_punch = false;
  auto out = Run(req);
  // Inside this fixture's shortened await budget (300 ms; production extends it past the peer's
  // dial overlap + circuit budget): the offerer's circuit arrives while we wait, unpunched.
  std::thread peer([this] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    dial_->Connect(kPeer, /*carrier=*/true, /*hop=*/true);
  });
  const bool done = WaitDone(out, std::chrono::seconds(10));
  peer.join();
  ASSERT_TRUE(done);
  ASSERT_TRUE(*out->result) << out->result->error().message;
  EXPECT_EQ(circuit_->calls.load(), 0) << "no punch";
  EXPECT_EQ(dial_->ensure_calls.load(), 0);
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
}

// TX-only escalate: exclude_direct builds a circuit even from Await and over a connected link.
TEST_F(PeerReachCoordinatorTest, ExcludeDirectForcesCircuit) {
  dial_->Connect(kPeer);
  dial_->endpoints[kPeer] = kPublicMa;
  auto req = Request(PeerReachMode::Await);
  req.exclude_direct = true;
  auto out = Run(req);
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(10)));
  ASSERT_TRUE(*out->result) << out->result->error().message;
  EXPECT_EQ(circuit_->calls.load(), 1);
  EXPECT_TRUE(circuit_->last_allow_circuit.load());
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
  EXPECT_FALSE((*out->result)->reused_link);
}

// #235: the reacher's own circuit missed, then the peer's relay link landed. The reacher waits the
// peer-dial overlap for it (as the awaiting side does) instead of failing on its spent dial budget.
TEST_F(PeerReachCoordinatorTest, ReachWaitsForThePeersLinkAfterItsCircuitMisses) {
  dial_->endpoints[kPeer] = kPublicMa;
  circuit_->connects = false;
  auto out = Run(Request(PeerReachMode::Reach));
  std::thread peer([this, out] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (circuit_->calls.load() == 0 && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Inside the fixture's shortened peer window (300 ms; production waits kPeerDialOverlapMs).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dial_->Connect(kPeer, /*carrier=*/true, /*hop=*/true);
  });
  const bool done = WaitDone(out, std::chrono::seconds(10));
  peer.join();
  ASSERT_TRUE(done);
  ASSERT_TRUE(*out->result) << "gave up at the circuit miss: " << out->result->error().message;
  EXPECT_EQ((*out->result)->kind, PeerLinkKind::Relayed);
  EXPECT_EQ(circuit_->calls.load(), 1);
}

// A relay standby (exclude_direct) whose circuit misses must fail — never "succeed" by dialing the
// peer directly: that only reaches the direct link the call is already on (hard-lab flip).
TEST_F(PeerReachCoordinatorTest, ExcludeDirectNeverSettlesOnADirectDial) {
  dial_->Connect(kPeer);  // the call's direct link
  dial_->endpoints[kPeer] = kPublicMa;
  dial_->ensure_connects = true;
  circuit_->connects = false;
  auto req = Request(PeerReachMode::Reach);
  req.exclude_direct = true;
  auto out = Run(req);
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(10)));
  ASSERT_FALSE(*out->result) << "settled as " << PeerLinkKindName((*out->result)->kind);
  EXPECT_EQ(dial_->ensure_calls.load(), 0) << "no direct dial for a relay-only reach";
  EXPECT_EQ(circuit_->calls.load(), 1);
}

TEST_F(PeerReachCoordinatorTest, UnreachablePeerFailsWithLastError) {
  dial_->endpoints[kPeer] = kPublicMa;
  circuit_->connects = false;
  auto out = Run(Request(PeerReachMode::Reach));
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(30)));
  ASSERT_FALSE(*out->result);
  EXPECT_FALSE(out->result->error().message.empty());
}

TEST_F(PeerReachCoordinatorTest, CancelCompletesInlineOnce) {
  dial_->endpoints[kPeer] = kPublicMa;  // never connects: dial misses, circuit stays pending
  circuit_->connects = false;
  std::atomic<int> completions{0};
  const PeerReachId id =
      reach_->Ensure(Request(), [&completions](Roe<PeerReachResult> r) {
        EXPECT_FALSE(r);
        completions.fetch_add(1);
      });
  reach_->Cancel(id);
  EXPECT_EQ(completions.load(), 1) << "Cancel completes inline";
  reach_->Cancel(id);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  EXPECT_EQ(completions.load(), 1) << "no second completion from queued steps";
}

TEST_F(PeerReachCoordinatorTest, MissingDialRegistryFails) {
  reach_->SetDeps(nullptr, circuit_.get());
  auto out = Run(Request());
  ASSERT_TRUE(WaitDone(out, std::chrono::seconds(5)));
  ASSERT_FALSE(*out->result);
}

// --- Link-state operations ---------------------------------------------------------------

// Dogfood 7bd62: a stale account: alias looked dialable while the Connected link lived under the
// PeerId. Prefer the PeerId, teaching it the alias's Preferred multiaddr when needed.
TEST_F(PeerReachCoordinatorTest, PreferDialKeyCopiesAliasEndpointOntoPeerId) {
  dial_->endpoints["account:peer"] = kPublicMa;
  EXPECT_EQ(reach_->PreferDialKey("account:peer", kPeer), kPeer);
  std::lock_guard lock(dial_->mu);
  EXPECT_EQ(dial_->endpoints[kPeer], kPublicMa);
}

TEST_F(PeerReachCoordinatorTest, PreferDialKeyPrefersDialablePeerId) {
  dial_->endpoints[kPeer] = kPrivateMa;
  dial_->endpoints["account:peer"] = kPublicMa;
  EXPECT_EQ(reach_->PreferDialKey("account:peer", kPeer), kPeer);
  std::lock_guard lock(dial_->mu);
  EXPECT_EQ(dial_->endpoints[kPeer], kPrivateMa) << "a dialable PeerId keeps its own endpoint";
}

TEST_F(PeerReachCoordinatorTest, PreferDialKeyUsesPeerIdWhenNeitherDialable) {
  EXPECT_EQ(reach_->PreferDialKey("account:peer", kPeer), kPeer);
  EXPECT_EQ(reach_->PreferDialKey("account:peer", ""), "account:peer");
}

TEST_F(PeerReachCoordinatorTest, ForgetPathClearsRelayHopAndBackoff) {
  dial_->Connect(kPeer, /*carrier=*/true, /*hop=*/true);
  ASSERT_TRUE(reach_->HasRelayHop(kPeer));
  reach_->ForgetPath(kPeer);
  EXPECT_FALSE(reach_->HasRelayHop(kPeer));
  EXPECT_EQ(dial_->clear_backoff_calls.load(), 1);
  EXPECT_TRUE(dial_->IsConnected(kPeer)) << "forgetting the path does not drop the link";
}

TEST_F(PeerReachCoordinatorTest, ReleasePeerAbortsDialAndDropsRelayHop) {
  dial_->Connect(kPeer, /*carrier=*/true, /*hop=*/true);
  reach_->ReleasePeer(kPeer);
  EXPECT_EQ(dial_->abort_calls.load(), 1);
  EXPECT_FALSE(reach_->HasRelayHop(kPeer));
}

TEST_F(PeerReachCoordinatorTest, AvailabilityFollowsDeps) {
  EXPECT_TRUE(reach_->Available());
  EXPECT_TRUE(reach_->HasCircuitReach());
  reach_->SetDeps(nullptr, nullptr);
  EXPECT_FALSE(reach_->Available());
  EXPECT_FALSE(reach_->HasCircuitReach());
  EXPECT_FALSE(reach_->HasRelayHop(kPeer));
  EXPECT_EQ(reach_->PreferDialKey("account:peer", kPeer), kPeer);
  reach_->ForgetPath(kPeer);   // no-ops without deps
  reach_->ReleasePeer(kPeer);
  reach_->AbortCircuitAttempts();
}

} // namespace
} // namespace pbr
