#pragma once

#include "amp/link/PeerLinkManager.h"
#include "common/Module.h"
#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "foundation/runtime/DeferredSelf.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct CircuitRendezvousDeps {
  std::function<MeshHost*()> mesh;
  /** Rendezvous surface (contacts ∪ directory ∪ DHT ∪ seeds); self / target are filtered here. */
  std::function<std::vector<MeshHopCandidate>()> rendezvous_candidates;
  /** Effective bootstrap seeds (configured ∪ directory). */
  std::function<std::vector<MeshHopCandidate>()> bootstrap_seeds;
  /** Circuit reach's last good relay — the sticky R1 when none was chosen / announced. */
  std::function<std::string()> last_good_relay;
};

/**
 * Circuit rendezvous, both directions of the same relay surface (H011):
 * - **dial side**: which relays circuit reach may StartBridge through (`DialableRelayIds`);
 * - **park side**: make this node ServeDial-reachable through them — warm seeds, StartReserve over
 *   the surface (connected first, then a bounded serial cold walk), late-reserve a chosen R1, await
 *   a seed park (H010), and re-park when a surface peer reconnects after a path change (B27).
 *
 * Threading: entry points on any thread; association / reserve work posts to the Amp IO thread;
 * callbacks capturing `this` are dropped after `Invalidate` (a pending park still answers `false`
 * at its deadline).
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
  /** Reserve, then on_done(true) once seeds are Connected, or false at timeout (H010). */
  void EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, int timeout_ms = 12000);
  /** Blocking EnsureBootstrapSeedParkedAsync — workers only (MeshPump drives progress). */
  bool AwaitCircuitReady(int timeout_ms = 12000);

  /** B27: arm re-park on reconnect for the running mesh (after the owner wires it). */
  void InstallReparkListener();
  /** Drop async callbacks and the re-park listener (mesh stop / teardown). */
  void Invalidate();
  void Clear();

private:
  struct SeedPark;
  struct ColdPark;

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  void PostIoOrRun(std::function<void()> task) const;
  std::vector<MeshHopCandidate> RendezvousCandidates(const std::string& exclude_peer_id = {}) const;
  std::vector<std::string> BootstrapSeedPeerIds() const;
  bool AnyBootstrapSeedConnectedOnIo() const;
  bool AllBootstrapSeedsConnectedOnIo() const;

  void WarmBootstrapSeedSessionsOnIo();
  void ReserveOnBootstrapSeedsOnIo();
  /** Rendezvous surface registered and ordered for parking (sticky R1 / connected first). */
  std::vector<MeshHopCandidate> OrderedParkSurface(IChatPeerLinks& links);
  void StartReserveOnRelay(const std::string& relay, const char* label);
  void ReserveColdSurface(const std::shared_ptr<ColdPark>& park, size_t index, size_t cold_started);
  void PreferLateReserveOnIo(const std::string& relay_peer_id);
  void CheckSeedPark(const std::shared_ptr<SeedPark>& park, bool at_deadline);
  static void FinishSeedPark(const std::shared_ptr<SeedPark>& park, bool parked);
  static void AbandonSeedPark(const std::shared_ptr<SeedPark>& park);
  void RemoveReparkListener();
  void OnRendezvousPeerReconnected(const std::string& peer_id);

  CircuitRendezvousDeps deps_;
  /** H011 L3.1b/c: last chosen / announced R1 PeerId for park sticky + late-reserve. */
  std::string chosen_circuit_r1_;
  pp::amp::PeerLinkManager::PeerConnectedListenerId repark_listener_id_ = 0;
  DeferredSelf deferred_;
};

} // namespace pbr
