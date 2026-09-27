#pragma once

#include "amp/link/PeerLinkManager.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/l4/media_relay/IMediaRelayClient.h"
#include "domain/mesh/l4/media_relay/MediaRelayAttach.h"
#include "domain/mesh/reachability/MeshReachPorts.h"
#include "foundation/runtime/DeferredSelf.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Cold-punch introducers in preference order: contacts first, then configured seeds. */
struct MeshPunchIntroducers {
  std::vector<std::string> contact_peer_ids;
  std::vector<std::string> seed_peer_ids;
};

/**
 * What the plane needs from the product. Hop candidates are policy (contacts, directory, DHT,
 * seeds) the wiring feature computes — `domain/mesh` does not read contacts.
 */
struct MeshMediaPlaneDeps {
  std::function<MeshHost*()> mesh;
  /** Rendezvous surface (contacts ∪ directory ∪ DHT ∪ seeds); self / target are filtered here. */
  std::function<std::vector<MeshHopCandidate>()> rendezvous_candidates;
  /** Effective bootstrap seeds (configured ∪ directory). */
  std::function<std::vector<MeshHopCandidate>()> bootstrap_seeds;
  std::function<MeshPunchIntroducers()> punch_introducers;
  /** A peer listen addr was registered (LAN discovery bookkeeping). */
  std::function<void(const std::string& peer_id)> note_lan_peer_id;
  /** Without a dial registry: record one direct endpoint (delivery plane). */
  std::function<void(const std::string& key, const std::string& multiaddr)> register_direct_endpoint;
};

/**
 * Neutral mesh media objects shared by calls and broadcast (media-client-layers L015): the
 * media_relay client, dial registry + peer listen book, circuit / service reach (with cold and
 * upgrade punch) and rendezvous parking so this node is reachable through org hops.
 *
 * The owner sequences rewires: dependents holding `RelayClient()` / `Dial()` / `CircuitReach()`
 * must be detached before `Wire`, `ResetRelayClients`, `ResetRelayClient` or teardown replace them.
 *
 * Threading: owner calls on the UI / control thread; parking work runs on the Amp IO thread;
 * async callbacks that capture `this` are dropped after `InvalidateAsyncOps`.
 */
class MeshMediaPlane : public Module {
public:
  using SignalingPunchFn = std::function<void(const std::string& target_peer_id,
                                              const std::vector<std::string>& my_addrs,
                                              std::function<void(Roe<void>)> on_done)>;

  MeshMediaPlane();
  ~MeshMediaPlane() override;
  MeshMediaPlane(const MeshMediaPlane&) = delete;
  MeshMediaPlane& operator=(const MeshMediaPlane&) = delete;

  void SetDeps(MeshMediaPlaneDeps deps);
  /** Last-resort punch through a consumer's own signaling when Amp introducers are exhausted (H012). */
  void SetSignalingPunch(SignalingPunchFn punch);
  /** Circuit reach chose a rendezvous relay (H011: calls announce it to the call peer). */
  void SetOnRelayChosen(std::function<void(const std::string& relay_peer_id)> callback);

  /** (Re)create relay client, dial registry and circuit reach from the running mesh. */
  void Wire();
  /** Tests / harness without MeshHost objects: use these instead (not owned). */
  void BindTestPath(IDialRegistry* dial, ICircuitHopReach* circuit_reach);
  /** Drop async callbacks and the re-park listener (mesh stop / teardown). */
  void InvalidateAsyncOps();
  void ResetRelayClient();
  /** Relay client + dial registry (capability refresh). */
  void ResetRelayClients();
  /** After the mesh stopped: dial registry and circuit reach (they referenced its links / tunnel). */
  void ResetAfterMeshStop();
  /** Everything, including the listen book (owner teardown). */
  void Clear();

  IMediaRelayClient* RelayClient() const { return media_relay_client_.get(); }
  IDialRegistry* Dial() const;
  ICircuitHopReach* CircuitReach() const;
  /** media_relay client + dial + service reach, for `AttachToMediaRelayAsync` users. */
  MediaRelayAttachPorts RelayAttachPorts() const;
  /** True when the mesh runs a started Amp media_relay coordinator. */
  bool AmpRelayAvailable() const;

  // --- peer listen book ---------------------------------------------------------------------
  /**
   * Rank, merge and register a peer's listen multiaddrs under `key` (account id or PeerId).
   * Returns the PeerId of the best dialable addr (empty when none was dialable).
   */
  std::string RegisterPeerListenMultiaddrs(const std::string& key, const std::vector<std::string>& multiaddrs);
  std::unordered_map<std::string, std::vector<std::string>> PeerListenBook() const { return peer_listen_mas_; }
  /** LAN-confirmed: a connected Amp link (mDNS / LAN dial). */
  bool PeerLanConfirmed(const std::string& peer_id) const;

  // --- reach --------------------------------------------------------------------------------
  Roe<void> TryEnsureCircuitHopReachable(const std::string& hop_peer_id);
  Roe<void> TryEnsurePeerReachable(const std::string& peer_key);
  void TryEnsurePeerReachableAsync(const std::string& peer_key, std::function<void(Roe<void>)> on_done);
  Roe<void> TryUpgradeToDirect(const std::string& peer_key);

  // --- rendezvous parking -------------------------------------------------------------------
  void WarmBootstrapSeedSessions();
  /** StartReserve over the rendezvous surface so ServeDial finds us through org hops (H011). */
  void ReserveOnBootstrapSeeds();
  /** StartReserve a specific R1 (chosen / announced by the peer); no-op when empty. */
  void PreferLateReserve(const std::string& relay_peer_id);
  /** Reserve, then on_done(true) once seeds are Connected, or false at timeout (H010). */
  void EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, int timeout_ms = 12000);
  /** Blocking EnsureBootstrapSeedParkedAsync — workers only (MeshPump drives progress). */
  bool AwaitCircuitReady(int timeout_ms = 12000);

private:
  struct SeedPark;
  struct ColdPark;

  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  void WireMediaRelayClient(MeshHost* m, const MeshIoContext& io);
  void WireDialRegistry(MeshHost* m, const MeshIoContext& io);
  void WireCircuitHopReach(MeshHost* m, const MeshIoContext& io);
  void PostIoOrRun(std::function<void()> task) const;

  void TryColdPunchAsync(const std::string& target_peer_id, std::function<void(Roe<void>)> on_done);
  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            std::function<void(Roe<void>)> on_done);

  std::vector<MeshHopCandidate> RendezvousCandidates(const std::string& exclude_peer_id = {}) const;
  std::vector<std::string> CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const;
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

  /** B27: a rendezvous peer reconnected after a path change — re-StartReserve on it. */
  void InstallRendezvousReparkListener();
  void RemoveRendezvousReparkListener();
  void OnRendezvousPeerReconnected(const std::string& peer_id);

  MeshMediaPlaneDeps deps_;
  SignalingPunchFn signaling_punch_;
  std::function<void(const std::string&)> on_relay_chosen_;
  std::unordered_map<std::string, std::vector<std::string>> peer_listen_mas_;

  std::unique_ptr<IMediaRelayClient> media_relay_client_;
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<ICircuitHopReach> circuit_hop_reach_;
  IDialRegistry* test_dial_ = nullptr;
  ICircuitHopReach* test_circuit_reach_ = nullptr;
  /** H011 L3.1b/c: last chosen / announced R1 PeerId for park sticky + late-reserve. */
  std::string chosen_circuit_r1_;
  pp::amp::PeerLinkManager::PeerConnectedListenerId repark_listener_id_ = 0;
  DeferredSelf deferred_;
};

} // namespace pbr
