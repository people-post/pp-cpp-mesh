#pragma once

#include "amp/link/PeerLinkManager.h"
#include "common/Lifetime.h"
#include "common/Module.h"
#include "mesh/host/MeshHost.h"
#include "mesh/reach/PeerReachRecord.h"
#include "mesh/reach/PeerReachSelect.h"
#include "mesh/types/MeshHopTypes.h"
#include "mesh/types/ServiceLoad.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
namespace pp::mesh {

struct CircuitRendezvousDeps {
  std::function<MeshHost*()> mesh;
  /** Rendezvous surface (contacts ∪ directory ∪ DHT ∪ seeds); self / target are filtered here. */
  std::function<std::vector<MeshHopCandidate>()> rendezvous_candidates;
  /** Effective bootstrap seeds (configured ∪ directory). */
  std::function<std::vector<MeshHopCandidate>()> bootstrap_seeds;
  /** Circuit reach's last good relay — the sticky R1 when none was chosen / announced. */
  std::function<std::string()> last_good_relay;
  /** The app's last good relays from earlier runs, best first (MeshHopPolicy::last_good_relays); none: none. */
  std::function<std::vector<std::string>()> last_good_relays;
  /** What the reach record says about a peer on the current network (none: every peer Unknown). */
  std::function<PeerReachFacts(const std::string& peer_id)> reach_facts;
};

/**
 * Circuit rendezvous, both directions of the same relay surface (H011):
 * - **dial side**: which relays circuit reach may StartBridge through (`DialableRelayIds`);
 * - **park side**: make this node ServeDial-reachable through them — warm seeds, StartReserve over
 *   the surface (connected first, then a bounded serial cold walk), late-reserve a chosen R1, await
 *   a seed park (H010), and re-park when a surface peer reconnects after a path change (B27).
 *
 * Both sides choose from the reach record (docs/PEER_REACH.md § 5): a relay it holds down is not
 * dialed, the cold walk (seeds first) stops at kRendezvousLiveTarget live relays and spends at most
 * kReachMaxColdDialsPerRound dials, and a park waits for that many seeds — answering at once when
 * every seed is held down.
 *
 * Threading: entry points on any thread; association / reserve work, park checks and their timers
 * (Amp's clock) run on the mesh's io lane; callbacks capturing `this` are dropped after
 * `Invalidate`, which answers every pending park `false` at once (the stopping runtime drops its
 * timers).
 */
class CircuitRendezvousCoordinator : public Module {
public:
  CircuitRendezvousCoordinator();
  ~CircuitRendezvousCoordinator() override;
  CircuitRendezvousCoordinator(const CircuitRendezvousCoordinator&) = delete;
  CircuitRendezvousCoordinator& operator=(const CircuitRendezvousCoordinator&) = delete;

  void SetDeps(CircuitRendezvousDeps deps);
  /** Circuit reach chose (or a peer announced) this R1: park sticks to it first. */
  void NoteChosenRelay(const std::string& relay_peer_id);
  /** Relays circuit reach may dial through, dialable now (registers safe endpoints on the way). */
  std::vector<std::string> DialableRelayIds(const std::string& exclude_peer_id) const;

  void WarmBootstrapSeedSessions();
  /** StartReserve over the rendezvous surface so ServeDial finds us through org hops. */
  void ReserveOnBootstrapSeeds();
  /** StartReserve a specific R1 (chosen / announced by the peer); no-op when empty. */
  void PreferLateReserve(const std::string& relay_peer_id);
  /**
   * Reserve, then on_done(true) once enough bootstrap seeds are Connected (the target, or every one
   * not held down when fewer), false at timeout or at once when the record holds every one down
   * (H010). Only seeds count and only seeds are redialed — another rendezvous relay may be the very
   * peer being reached, which a park must never dial.
   */
  void EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, int timeout_ms = 12000);

  /** B27: arm re-park on reconnect for the running mesh (after the owner wires it). */
  void InstallReparkListener();
  /** Drop async callbacks and the re-park listener, and answer pending parks `false` (mesh stop / teardown). */
  void Invalidate();
  void Clear();

private:
  struct SeedPark;
  struct ColdPark;

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  void PostIoOrRun(std::function<void()> task) const;
  std::vector<MeshHopCandidate> RendezvousCandidates(const std::string& exclude_peer_id = {}) const;
  PeerReachFacts Facts(const std::string& peer_id) const;
  /**
   * `relay` counts toward the park's live relays by what it said of its own
   * load (docs/PEER_REACH.md § Service load): not when draining, nor when full
   * unless this device already holds its reservation there (a renewal is
   * accepted).
   */
  bool ParkCountsOn(const std::string& relay) const;
  /** The park's level for `relay`, as it said it (Unknown when it did not or
   * long ago). */
  ServiceLoad ParkLoad(const std::string& relay) const;
  /** A relay said full or draining: park elsewhere now, not at the next round
   * (at most every kReplaceParkEveryMs). */
  void ReplaceParkSoon(const std::string& relay, ServiceLoad load);
  static constexpr int64_t kReplaceParkEveryMs = 5000;
  /**
   * How long a park's reservation is held (the relay's own default; at most 60 s there). Longer than the app's
   * renewal (every 10 s in a call, every 20 s for a desktop staying reachable): at 15 s a desktop was unparked 5 s in
   * every 20 (pp-browser stress lab, 2026-11-06).
   */
  static constexpr int kParkReserveHoldMs = 30000;
  int64_t replace_park_at_ms_ = 0;
  /** The relay this node parks on first: the chosen / announced R1, else circuit reach's last good one. */
  std::string StickyRelay() const;
  std::vector<std::string> BootstrapSeedPeerIds() const;
  /**
   * The park's cold order: seeds first (the park waits on them), within each band the app's last good
   * relays first (in its order), then open before busy.
   */
  void OrderColdWalk(std::vector<ReachCandidate>& view) const;
  /** Bootstrap seeds Connected (carrier-only ones never count) and not held down, on the io lane. */
  struct ParkCount {
    size_t usable = 0;
    size_t connected = 0;
    int dials_in_flight = 0;
  };
  /** `verifying`: seeds a park is checking (Connected, not counted until they answer — § 8). */
  ParkCount CountParkOnIo(const std::unordered_set<std::string>* verifying = nullptr) const;
  /** A park's first check: ask every Connected seed heard from too long ago to prove it is alive. */
  void VerifyParkSeedsOnIo(const std::shared_ptr<SeedPark>& park);

  void WarmBootstrapSeedSessionsOnIo();
  void ReserveOnBootstrapSeedsOnIo();
  /** Rendezvous surface registered and ordered for parking (sticky R1 / connected first). */
  std::vector<MeshHopCandidate> OrderedParkSurface(IChatPeerLinks& links);
  void StartReserveOnRelay(const std::string& relay, const char* label);
  /**
   * Dial the park's next cold relay and reserve on it once it associates, until enough are live. One that has
   * not answered within kColdParkStagger does not hold up the next (Happy Eyeballs): an unreachable relay used to
   * cost its whole dial timeout (8 s) before the next was tried, and a call's caller had walked its relays and
   * given up before the answerer was parked anywhere (pp-browser #502, target_not_reserved).
   */
  void ReserveColdSurface(const std::shared_ptr<ColdPark>& park);
  /** A dial to `hop` missed: when a park waits on it, dial it again if the miss may pass (or fail the park). */
  void OnSeedDialMissed(IChatPeerLinks& links, const MeshHopCandidate& hop);
  std::vector<std::shared_ptr<SeedPark>> WaitingParks();
  void PreferLateReserveOnIo(const std::string& relay_peer_id);
  void CheckSeedPark(const std::shared_ptr<SeedPark>& park, bool at_deadline);
  static void FinishSeedPark(const std::shared_ptr<SeedPark>& park, bool parked);
  static void AbandonSeedPark(const std::shared_ptr<SeedPark>& park);
  void RemoveReparkListener();
  void OnRendezvousPeerReconnected(const std::string& peer_id);

  CircuitRendezvousDeps deps_;
  /** H011 L3.1b/c: last chosen / announced R1 PeerId for park sticky + late-reserve (IO strand). */
  std::string chosen_circuit_r1_;
  pp::amp::PeerLinkManager::PeerConnectedListenerId repark_listener_id_ = 0;
  /** Parks still waiting (Invalidate answers them). */
  std::mutex parks_mu_;
  std::vector<std::weak_ptr<SeedPark>> parks_;
  pp::Lifetime deferred_;
};

} // namespace pp::mesh
