#pragma once

#include "amp/link/PeerLinkManager.h"
#include "common/Module.h"
#include "mesh/types/MeshHopTypes.h"
#include "mesh/host/MeshHost.h"
#include "mesh/reach/PeerReachRecord.h"
#include "common/Lifetime.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
 * dialed, the cold walk stops at kRendezvousLiveTarget live relays and spends at most
 * kReachMaxColdDialsPerRound dials, and a park waits for that many — answering at once when every
 * relay is held down.
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
   * Reserve, then on_done(true) once enough rendezvous relays are Connected (the target, or every
   * one not held down when fewer), false at timeout or at once when the record holds every one down
   * (H010).
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
  /** The relay this node parks on first: the chosen / announced R1, else circuit reach's last good one. */
  std::string StickyRelay() const;
  /** Rendezvous relays Connected (carrier-only ones never count) and not held down, on the io lane. */
  struct ParkCount {
    size_t usable = 0;
    size_t connected = 0;
    int dials_in_flight = 0;
  };
  ParkCount CountParkOnIo() const;

  void WarmBootstrapSeedSessionsOnIo();
  void ReserveOnBootstrapSeedsOnIo();
  /** Rendezvous surface registered and ordered for parking (sticky R1 / connected first). */
  std::vector<MeshHopCandidate> OrderedParkSurface(IChatPeerLinks& links);
  void StartReserveOnRelay(const std::string& relay, const char* label);
  void ReserveColdSurface(const std::shared_ptr<ColdPark>& park, size_t index);
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
