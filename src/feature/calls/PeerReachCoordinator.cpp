#include "feature/calls/PeerReachCoordinator.h"

#include "domain/mesh/l4/circuit/CircuitServeDialPolicy.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/directory/MeshHopDial.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Direct-dial (EnsureAssociation) budget before circuit / punch pivot (V049 / B31). */
constexpr int64_t kDialBudgetMs = 12000;
/**
 * AmpCircuitHopReach envelope (H010 / kCircuitReachEnvelopeMs) + nest settle slack. Once a
 * circuit Ensure has started, the reach deadline is at least this far from circuit start.
 */
constexpr int64_t kCircuitEnsureBudgetMs = 16000;
/**
 * Extra wait so the peer's own simultaneous dial (V049) can land before we give up. Await mode
 * holds this window open after its punch; the hard watchdog covers it for both modes.
 */
constexpr int64_t kPeerDialOverlapMs = 15000;
constexpr int kPollMs = 250;
/** Backoff between EnsureAssociation attempts within the dial budget (B31 / V049). */
constexpr int kAssocRedialBackoffMs = 1500;
/**
 * Await bootstrap seed Connected before circuit / punch (H010). Must cover a full Amp hop dial
 * (~8 s) — 4 s parked=0 while the hop dial was still in flight (dogfood fd4e3de).
 */
constexpr int kSeedParkAwaitMs = 12000;

} // namespace

const char* PeerLinkKindName(const PeerLinkKind kind) {
  switch (kind) {
  case PeerLinkKind::Unknown:
    return "unknown";
  case PeerLinkKind::Direct:
    return "direct";
  case PeerLinkKind::Punched:
    return "punched";
  case PeerLinkKind::Relayed:
    return "relayed";
  }
  return "?";
}

/** One Ensure. Fields below `settled` are Coordinator-strand only. */
struct PeerReachCoordinator::Attempt {
  PeerReachId id = 0;
  PeerReachRequest req;
  Done on_done;
  SeedParkAwait seed_park;
  int64_t dial_budget_ms = kDialBudgetMs;
  std::atomic<bool> settled{false};

  const std::string& Primary() const { return req.keys.front(); }
  bool IsAwait() const { return req.mode == PeerReachMode::Await; }

  int64_t initial_deadline_ms = 0;
  int64_t deadline_ms = 0;
  Error last_error{"peer not connected"};
  bool circuit_started = false;
  bool circuit_inflight = false;
  bool assoc_started = false;
  bool assoc_done = false;
  int assoc_attempts = 0;
  bool seed_park_preassoc_done = false;
  bool seed_park_ok = false;
};

PeerReachCoordinator::PeerReachCoordinator(IDialRegistry* dial, ICircuitHopReach* circuit)
    : dial_(dial), circuit_(circuit), dial_budget_ms_(kDialBudgetMs),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
  redirectLogger("PeerReach");
}

PeerReachCoordinator::~PeerReachCoordinator() {
  alive_->store(false, std::memory_order_release);
  // The owner is going away: drop pending callbacks rather than completing into it. Owners
  // Cancel explicitly first (teardown) when they need completion.
  std::lock_guard lock(mu_);
  for (auto& [id, a] : attempts_) {
    a->settled.store(true, std::memory_order_release);
  }
  attempts_.clear();
}

void PeerReachCoordinator::SetDeps(IDialRegistry* dial, ICircuitHopReach* circuit) {
  dial_.store(dial, std::memory_order_release);
  circuit_.store(circuit, std::memory_order_release);
}

void PeerReachCoordinator::SetSeedParkAwait(SeedParkAwait park) {
  std::lock_guard lock(mu_);
  seed_park_ = std::move(park);
}

void PeerReachCoordinator::SetDialBudgetMsForTest(const int budget_ms) {
  std::lock_guard lock(mu_);
  dial_budget_ms_ = budget_ms > 0 ? budget_ms : kDialBudgetMs;
}

PeerReachId PeerReachCoordinator::Ensure(PeerReachRequest request, Done on_done) {
  auto a = std::make_shared<Attempt>();
  a->req = std::move(request);
  a->on_done = std::move(on_done);
  {
    std::lock_guard lock(mu_);
    a->id = next_id_++;
    a->seed_park = seed_park_;
    a->dial_budget_ms = dial_budget_ms_;
    attempts_[a->id] = a;
  }
  AppRuntime::PostCoordinatorNormal([this, alive = alive_, a]() {
    if (alive->load(std::memory_order_acquire)) {
      Start(a);
    }
  });
  return a->id;
}

void PeerReachCoordinator::AbandonDial(const std::string& key) {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  if (!dial || key.empty()) {
    return;
  }
  dial->AbortInflightDial(key);
  dial->ClearDialBackoff(key);
  if (auto ma = dial->PreferredMultiaddr(key)) {
    log().info << "abandon dial peer=" << key << " ma=" << *ma;
  } else {
    log().info << "abandon dial peer=" << key << " ma=(none) dialable=" << (dial->IsDialable(key) ? 1 : 0);
  }
}

bool PeerReachCoordinator::Available() const {
  return dial_.load(std::memory_order_acquire) != nullptr;
}

bool PeerReachCoordinator::HasCircuitReach() const {
  return circuit_.load(std::memory_order_acquire) != nullptr;
}

bool PeerReachCoordinator::HasRelayHop(const std::string& key) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  return dial && !key.empty() && dial->HasCallMediaCircuitHop(key);
}

void PeerReachCoordinator::ForgetPath(const std::string& key) {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  if (!dial || key.empty()) {
    return;
  }
  dial->ClearCallMediaCircuitHop(key);
  dial->ClearDialBackoff(key);
}

void PeerReachCoordinator::ReleasePeer(const std::string& key) {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  if (!dial || key.empty()) {
    return;
  }
  dial->AbortInflightDial(key);
  dial->ClearCallMediaCircuitHop(key);
}

void PeerReachCoordinator::AbortCircuitAttempts() {
  if (ICircuitHopReach* circuit = circuit_.load(std::memory_order_acquire)) {
    circuit->AbortPending();
  }
}

std::string PeerReachCoordinator::PreferDialKey(const std::string& alias, const std::string& peer_id) {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  if (peer_id.empty()) {
    return alias;
  }
  if (!dial) {
    return peer_id;
  }
  const bool alias_dialable = dial->IsDialable(alias);
  if (alias_dialable && !dial->IsDialable(peer_id)) {
    if (auto ma = dial->PreferredMultiaddr(alias)) {
      (void)dial->RegisterEndpoint(peer_id, *ma);
    }
  }
  const bool peer_dialable = dial->IsDialable(peer_id);
  log().info << "dial key alias=" << alias << " peer_id=" << peer_id
             << " alias_dialable=" << (alias_dialable ? 1 : 0) << " peer_dialable=" << (peer_dialable ? 1 : 0);
  if (peer_dialable || !alias_dialable) {
    return peer_id;
  }
  log().info << "dial key keep alias (PeerId still undialable) alias=" << alias;
  return alias;
}

void PeerReachCoordinator::Cancel(const PeerReachId id) {
  AttemptPtr a;
  {
    std::lock_guard lock(mu_);
    if (auto it = attempts_.find(id); it != attempts_.end()) {
      a = it->second;
    }
  }
  if (a) {
    Finish(a, Error("peer reach cancelled"));
  }
}

void PeerReachCoordinator::CancelAll() {
  std::unordered_map<PeerReachId, AttemptPtr> all;
  {
    std::lock_guard lock(mu_);
    all = attempts_;
  }
  for (auto& [id, a] : all) {
    Finish(a, Error("peer reach cancelled"));
  }
}

void PeerReachCoordinator::Finish(const AttemptPtr& a, Roe<PeerReachResult> result) {
  if (a->settled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    attempts_.erase(a->id);
  }
  Done done = std::move(a->on_done);
  if (done) {
    done(std::move(result));
  }
}

void PeerReachCoordinator::ScheduleTick(const AttemptPtr& a, const int delay_ms) {
  (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(delay_ms),
                                               [this, alive = alive_, a]() {
                                                 if (alive->load(std::memory_order_acquire)) {
                                                   Tick(a);
                                                 }
                                               });
}

bool PeerReachCoordinator::AnyConnected(const Attempt& a) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  return dial && std::any_of(a.req.keys.begin(), a.req.keys.end(),
                             [dial](const std::string& k) { return dial->IsConnected(k); });
}

bool PeerReachCoordinator::AnyConnectedDirect(const Attempt& a) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  return dial && std::any_of(a.req.keys.begin(), a.req.keys.end(),
                             [dial](const std::string& k) { return dial->IsConnectedDirect(k); });
}

bool PeerReachCoordinator::AnyDialable(const Attempt& a) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  return dial && std::any_of(a.req.keys.begin(), a.req.keys.end(),
                             [dial](const std::string& k) { return dial->IsDialable(k); });
}

bool PeerReachCoordinator::AnyCircuitHop(const Attempt& a) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  return dial && std::any_of(a.req.keys.begin(), a.req.keys.end(),
                             [dial](const std::string& k) { return dial->HasCallMediaCircuitHop(k); });
}

bool PeerReachCoordinator::PreferredIsPublic(const Attempt& a) const {
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  if (!dial) {
    return false;
  }
  for (const auto& k : a.req.keys) {
    if (auto ma = dial->PreferredMultiaddr(k); ma && !ma->empty()) {
      return MultiaddrHasPublicDialHost(*ma);
    }
  }
  return false;
}

void PeerReachCoordinator::ClearBackoff(const Attempt& a) {
  if (IDialRegistry* dial = dial_.load(std::memory_order_acquire)) {
    for (const auto& k : a.req.keys) {
      dial->ClearDialBackoff(k);
    }
  }
}

void PeerReachCoordinator::Start(const AttemptPtr& a) {
  if (a->settled.load(std::memory_order_acquire)) {
    return;
  }
  if (a->req.keys.empty() || a->Primary().empty()) {
    Finish(a, Error("missing peer key"));
    return;
  }
  if (!dial_.load(std::memory_order_acquire)) {
    Finish(a, Error("dial registry not available"));
    return;
  }
  const bool circuit = circuit_.load(std::memory_order_acquire) != nullptr;
  const bool connected = AnyConnected(*a);
  // IsDialable (has_endpoint) is not Connected — dogfood 19f845: a dialable skip then hung
  // OpenChannel on connected=0. Only a Connected link is reused.
  if (connected && !a->req.exclude_direct && !a->req.fresh_link) {
    PeerReachResult r;
    r.kind = AnyConnectedDirect(*a) ? PeerLinkKind::Direct : PeerLinkKind::Relayed;
    r.reused_link = true;
    log().info << "peer connected peer=" << a->Primary() << " path=" << PeerLinkKindName(r.kind)
               << " reused=1";
    Finish(a, r);
    return;
  }
  if (connected && a->req.fresh_link) {
    // B39: amp can still report the old link Connected after the peer changed network. Drop it
    // so the dial below uses fresh DialBook candidates.
    IDialRegistry* dial = dial_.load(std::memory_order_acquire);
    for (const auto& k : a->req.keys) {
      if (dial->IsConnected(k)) {
        dial->DropLink(k);
      }
    }
    log().info << "fresh link: dropped stale connected link peer=" << a->Primary();
  }
  a->seed_park_preassoc_done = !a->seed_park || !circuit;
  a->seed_park_ok = !a->seed_park || !circuit;
  log().info << "reach start peer=" << a->Primary()
             << " mode=" << (a->IsAwait() ? "await" : "reach")
             << " dialable=" << (AnyDialable(*a) ? 1 : 0) << " connected=" << (connected ? 1 : 0)
             << " exclude_direct=" << (a->req.exclude_direct ? 1 : 0)
             << " has_circuit_reach=" << (circuit ? 1 : 0) << " budget_ms=" << a->dial_budget_ms;

  a->initial_deadline_ms = util::NowUnixMs() + a->dial_budget_ms;
  a->deadline_ms = a->initial_deadline_ms;
  // Hard deadline independent of the tick chain (dogfood af934e: EnsureAssociation never called
  // back). Covers the peer's dial overlap (V049) + circuit slack. Must not touch dial state —
  // it can fire mid-StartBridge and race PeerLinkManager::FinishDial (dogfood 091029).
  (void)AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(a->dial_budget_ms + kPeerDialOverlapMs + kCircuitEnsureBudgetMs + 250),
      [this, alive = alive_, a]() {
        if (!alive->load(std::memory_order_acquire) || a->settled.load(std::memory_order_acquire)) {
          return;
        }
        log().warning << "reach deadline watchdog peer=" << a->Primary()
                      << " last=" << a->last_error.message;
        Finish(a, a->last_error);
      });
  Tick(a);
}

bool PeerReachCoordinator::TrySettleConnected(const AttemptPtr& a) {
  const bool mutating = a->circuit_inflight || (a->assoc_started && !a->assoc_done);
  const bool wait_for_circuit = a->req.exclude_direct && !a->circuit_started && !AnyCircuitHop(*a);
  if (mutating || wait_for_circuit || !AnyConnected(*a)) {
    return false;
  }
  PeerReachResult r;
  // The relay carrier also reads "connected" — only ADP counts as direct / punched.
  if (AnyCircuitHop(*a) || !AnyConnectedDirect(*a)) {
    r.kind = PeerLinkKind::Relayed;
  } else if (a->circuit_started) {
    r.kind = PeerLinkKind::Punched;
  } else {
    r.kind = PeerLinkKind::Direct;
  }
  log().info << "peer connected peer=" << a->Primary() << " path=" << PeerLinkKindName(r.kind);
  Finish(a, r);
  return true;
}

void PeerReachCoordinator::Tick(const AttemptPtr& a) {
  if (a->settled.load(std::memory_order_acquire)) {
    return;
  }
  if (!dial_.load(std::memory_order_acquire)) {
    Finish(a, Error("dial registry not available"));
    return;
  }
  // PeerLinkManager is Amp-IO only: do not read link state while our own EnsureAssociation or
  // circuit StartBridge is mutating it (dogfood 085210 / 091029). Hop registry reads are mutexed.
  if (TrySettleConnected(a)) {
    return;
  }
  if (util::NowUnixMs() >= a->deadline_ms) {
    // Never AbortPending a live StartBridge because punch / assoc burned the short budget.
    if (a->circuit_inflight) {
      ScheduleTick(a, kPollMs);
      return;
    }
    const bool circuit = circuit_.load(std::memory_order_acquire) != nullptr;
    if (circuit && !a->circuit_started && a->assoc_started && !a->assoc_done) {
      a->assoc_done = true;
      log().warning << "EnsureAssociation hung peer=" << a->Primary() << " pivoting to circuit";
    } else {
      log().warning << "peer still not connected peer=" << a->Primary()
                    << " last=" << a->last_error.message
                    << " circuit_started=" << (a->circuit_started ? 1 : 0)
                    << " assoc_started=" << (a->assoc_started ? 1 : 0);
      Finish(a, a->last_error);
      return;
    }
  }
  if (a->circuit_inflight || (a->assoc_started && !a->assoc_done)) {
    // Assoc / circuit callbacks drive progress; keep polling for the deadline only.
    ScheduleTick(a, kPollMs);
    return;
  }
  if (MaybeParkBeforePrivateDial(a) || MaybeStartAssociation(a) || MaybeStartCircuit(a)) {
    return;
  }
  ScheduleTick(a, kPollMs);
}

bool PeerReachCoordinator::MaybeParkBeforePrivateDial(const AttemptPtr& a) {
  // Dual-NAT: a private Preferred dial shares ADP UDP with the seed hop warm. Park on the seed
  // first so it owns the socket alone (dogfood fd4e3de). A timeout must not count as parked —
  // that skipped a real park before punch and ServeDial saw "endpoint not registered" (88e16f5c).
  if (a->seed_park_preassoc_done || !circuit_.load(std::memory_order_acquire) || PreferredIsPublic(*a)) {
    return false;
  }
  a->seed_park_preassoc_done = true;
  log().info << "seed park before private Preferred peer=" << a->Primary();
  a->seed_park(
      [this, alive = alive_, a](bool parked) {
        AppRuntime::PostCoordinatorNormal([this, alive, a, parked]() {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          a->seed_park_ok = parked;
          log().info << "seed park (pre-assoc) parked=" << (parked ? 1 : 0);
          Tick(a);
        });
      },
      kSeedParkAwaitMs);
  return true;
}

bool PeerReachCoordinator::MaybeStartAssociation(const AttemptPtr& a) {
  const bool wait_for_circuit = a->req.exclude_direct && !a->circuit_started && !AnyCircuitHop(*a);
  if (a->assoc_started || wait_for_circuit || !AnyDialable(*a)) {
    return false;
  }
  const bool circuit = circuit_.load(std::memory_order_acquire) != nullptr;
  const bool public_preferred = PreferredIsPublic(*a);
  // Await: never dial a private Preferred — that UDP path drops the seed link the peer's
  // ServeDial needs us Connected on (dogfood ae4900eb / 39412f). Reach: skip it only after a
  // successful seed park (H010 CircuitServeDialPolicy).
  const bool await_skip_private = a->IsAwait() && circuit && !public_preferred;
  if (await_skip_private || CallMediaShouldSkipPreferredDialAfterSeedPark(a->seed_park_ok, public_preferred)) {
    a->assoc_started = true;
    a->assoc_done = true;
    log().info << "skip EnsureAssociation private Preferred"
               << (await_skip_private ? " (await)" : " (seed parked)") << " peer=" << a->Primary();
    return false;  // fall through to circuit / punch
  }
  IDialRegistry* dial = dial_.load(std::memory_order_acquire);
  a->assoc_started = true;
  ++a->assoc_attempts;
  log().info << "EnsureAssociation start peer=" << a->Primary() << " attempt=" << a->assoc_attempts
             << " mode=" << (a->IsAwait() ? "await" : "reach");
  // V049 / B31: both modes dial (simultaneous open); Amp A026 elects one PeerLink.
  dial->EnsureAssociation(a->Primary(), [this, alive = alive_, a](Roe<void> assoc) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    // Runs on Amp IO (FinishDial) — snapshot link state before the Coordinator hop.
    const bool connected_now = static_cast<bool>(assoc) && AnyConnected(*a);
    const bool direct_now = connected_now && AnyConnectedDirect(*a);
    AppRuntime::PostCoordinatorNormal(
        [this, alive, a, assoc = std::move(assoc), connected_now, direct_now]() mutable {
          if (alive->load(std::memory_order_acquire)) {
            OnAssociationDone(a, std::move(assoc), connected_now, direct_now);
          }
        });
  });
  // Wait for assoc to finish before StartBridge so the two ADP handshakes never overlap.
  ScheduleTick(a, kPollMs);
  return true;
}

void PeerReachCoordinator::OnAssociationDone(const AttemptPtr& a, Roe<void> assoc, const bool connected_now,
                                             const bool direct_now) {
  if (a->settled.load(std::memory_order_acquire)) {
    return;
  }
  const auto rearm = [this, a]() {
    (void)AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kAssocRedialBackoffMs),
                                                 [this, alive = alive_, a]() {
                                                   if (!alive->load(std::memory_order_acquire)) {
                                                     return;
                                                   }
                                                   a->assoc_started = false;
                                                   a->assoc_done = false;
                                                   Tick(a);
                                                 });
  };
  if (assoc) {
    a->assoc_done = true;
    if (connected_now) {
      PeerReachResult r;
      r.kind = direct_now ? PeerLinkKind::Direct : PeerLinkKind::Relayed;
      log().info << "EnsureAssociation ok peer=" << a->Primary() << " path=" << PeerLinkKindName(r.kind);
      Finish(a, r);
      return;
    }
    // Handshake finished without a Connected link — treat like a miss for re-dial.
    log().info << "EnsureAssociation ok but not connected peer=" << a->Primary();
    ClearBackoff(*a);
    if (util::NowUnixMs() + kAssocRedialBackoffMs < a->deadline_ms) {
      a->assoc_done = false;
      rearm();
      return;
    }
  } else {
    a->last_error = assoc.error();
    log().info << "EnsureAssociation miss peer=" << a->Primary() << " err=" << a->last_error.message
               << " attempt=" << a->assoc_attempts;
    // The dial already finished (this callback): ClearDialBackoff only. AbortInflightDial on a
    // Backoff link schedules a second drop racing FinishDial (dogfood 130521).
    ClearBackoff(*a);
    // B31 / V049: re-dial within budget. assoc_done stays false so circuit / punch waits for the
    // next attempt (otherwise a 250 ms poll pivots before the peer opens its hole).
    if (util::NowUnixMs() + kAssocRedialBackoffMs < a->deadline_ms) {
      rearm();
      return;
    }
    a->assoc_done = true;  // budget spent — allow circuit / punch
  }
  ScheduleTick(a, kPollMs);
}

bool PeerReachCoordinator::MaybeStartCircuit(const AttemptPtr& a) {
  if (!circuit_.load(std::memory_order_acquire) || a->circuit_started) {
    return false;
  }
  const bool due = a->req.exclude_direct || !AnyDialable(*a) || (a->assoc_done && !AnyConnected(*a));
  if (!due) {
    return false;
  }
  a->circuit_started = true;
  a->circuit_inflight = true;
  // Await: punch only and wait for the peer's circuit. Building our own toward the reacher fails
  // with "endpoint not registered" when fleet seeds do not see it (dogfood 072a7425).
  const bool allow_circuit = !a->IsAwait() || a->req.exclude_direct;
  const int64_t now = util::NowUnixMs();
  if (allow_circuit) {
    a->deadline_ms = std::max(a->deadline_ms, now + kCircuitEnsureBudgetMs);
  } else if (a->dial_budget_ms >= kDialBudgetMs) {
    // Production await: cover the peer's dial overlap + circuit slack (gtests keep a short budget).
    a->deadline_ms = std::max(a->deadline_ms, now + kPeerDialOverlapMs + kCircuitEnsureBudgetMs);
  }
  log().info << "circuit/punch start peer=" << a->Primary() << " circuit_budget_ms=" << kCircuitEnsureBudgetMs
             << " allow_circuit=" << (allow_circuit ? 1 : 0);
  // Always confirm a Connected seed before StartBridge / punch-wait: the pre-assoc park may have
  // timed out, or a later private dial dropped the hop link (dogfood 88e16f5c).
  if (a->seed_park) {
    a->seed_park(
        [this, alive = alive_, a, allow_circuit](bool parked) {
          AppRuntime::PostCoordinatorNormal([this, alive, a, allow_circuit, parked]() {
            if (!alive->load(std::memory_order_acquire)) {
              return;
            }
            a->seed_park_ok = parked;
            log().info << "seed park before circuit/punch peer=" << a->Primary()
                       << " parked=" << (parked ? 1 : 0);
            KickCircuit(a, allow_circuit);
          });
        },
        kSeedParkAwaitMs);
  } else {
    KickCircuit(a, allow_circuit);
  }
  ScheduleTick(a, kPollMs);
  return true;
}

void PeerReachCoordinator::KickCircuit(const AttemptPtr& a, const bool allow_circuit) {
  if (a->settled.load(std::memory_order_acquire)) {
    a->circuit_inflight = false;
    return;
  }
  ICircuitHopReach* circuit = circuit_.load(std::memory_order_acquire);
  if (!circuit) {
    a->circuit_inflight = false;
    return;
  }
  circuit->TryEnsureCallMediaReachableAsync(
      a->Primary(),
      [this, alive = alive_, a](Roe<void> via) {
        if (!alive->load(std::memory_order_acquire)) {
          return;
        }
        // Finishes on Amp IO — snapshot before the Coordinator hop.
        const bool connected_now = AnyConnected(*a);
        // Connected only through a relay carrier is not a punch.
        const bool relayed_now = AnyCircuitHop(*a) || (connected_now && !AnyConnectedDirect(*a));
        AppRuntime::PostCoordinatorNormal(
            [this, alive, a, via = std::move(via), connected_now, relayed_now]() mutable {
              if (alive->load(std::memory_order_acquire)) {
                OnCircuitDone(a, std::move(via), connected_now, relayed_now);
              }
            });
      },
      allow_circuit);
}

void PeerReachCoordinator::OnCircuitDone(const AttemptPtr& a, Roe<void> via, const bool connected_now,
                                         const bool relayed_now) {
  a->circuit_inflight = false;
  if (a->settled.load(std::memory_order_acquire)) {
    return;
  }
  // has_endpoint / punch "ok" is not PeerLink Connected — dogfood 612b: via_ok=1, connected=0.
  if (connected_now) {
    PeerReachResult r;
    r.kind = relayed_now ? PeerLinkKind::Relayed : PeerLinkKind::Punched;
    log().info << "peer reachable via circuit/punch peer=" << a->Primary()
               << " path=" << PeerLinkKindName(r.kind) << " via_ok=" << (via ? 1 : 0);
    Finish(a, r);
    return;
  }
  a->last_error = via ? Error("peer not connected after circuit/punch") : via.error();
  log().info << "circuit/punch miss peer=" << a->Primary() << " err=" << a->last_error.message
             << " via_ok=" << (via ? 1 : 0);
  // Await, or the seed does not know the peer yet: keep the extended deadline for the peer's
  // link to land. Otherwise fall back to the short budget.
  const bool wait_inbound =
      a->IsAwait() || a->last_error.message.find("not registered") != std::string::npos;
  if (!wait_inbound) {
    a->deadline_ms = a->initial_deadline_ms;
    if (util::NowUnixMs() >= a->deadline_ms) {
      Finish(a, a->last_error);
      return;
    }
  }
  ScheduleTick(a, kPollMs);
}

} // namespace pbr
