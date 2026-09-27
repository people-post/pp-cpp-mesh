#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/l4/media_relay/IMediaRelayClient.h"
#include "domain/mesh/l4/media_relay/MediaRelayAttach.h"
#include "domain/mesh/reachability/CircuitRendezvousCoordinator.h"
#include "domain/mesh/reachability/MeshReachPorts.h"
#include "domain/mesh/reachability/PunchIntroducerWalk.h"
#include "foundation/runtime/DeferredSelf.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

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
 * Owner of the neutral mesh media objects shared by calls and broadcast (media-client-layers
 * L015): the media_relay client, dial registry + peer listen book, circuit / service reach, and the
 * reach pieces it is built from — `PunchIntroducerWalk` (punch step) and
 * `CircuitRendezvousCoordinator` (relay surface for dialing and parking). Composition and lifecycle
 * only: reach mechanics live in `domain/mesh/reachability`.
 *
 * The owner sequences rewires: dependents holding `RelayClient()` / `Dial()` / `CircuitReach()`
 * must be detached before `Wire`, `ResetRelayClients`, `ResetRelayClient` or teardown replace them.
 *
 * Threading: owner calls on the UI / control thread; async callbacks that capture `this` (and the
 * rendezvous coordinator's) are dropped after `InvalidateAsyncOps`.
 */
class MeshMediaPlane : public Module {
public:
  using SignalingPunchFn = PunchIntroducerWalk::SignalingPunchFn;

  MeshMediaPlane();
  ~MeshMediaPlane() override;
  MeshMediaPlane(const MeshMediaPlane&) = delete;
  MeshMediaPlane& operator=(const MeshMediaPlane&) = delete;

  void SetDeps(MeshMediaPlaneDeps deps);
  /** Last-resort punch through a consumer's own signaling when Amp introducers are exhausted (H012). */
  void SetSignalingPunch(SignalingPunchFn punch);
  /** Circuit reach chose a rendezvous relay (H011: calls announce it to the call peer). */
  void SetOnRelayChosen(std::function<void(const std::string& relay_peer_id)> callback);

  /** (Re)create relay client, dial registry and circuit reach from the running mesh; arm re-park. */
  void Wire();
  /** Tests / harness without MeshHost objects: use these instead (not owned). */
  void BindTestPath(IDialRegistry* dial, ICircuitHopReach* circuit_reach);
  /** Drop async callbacks, relay-chosen notices and the re-park listener (mesh stop / teardown). */
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

  /** Rendezvous relays: dial surface for reach, parking so this node is reachable (stable object). */
  CircuitRendezvousCoordinator& Rendezvous() { return rendezvous_; }

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  void WireMediaRelayClient(MeshHost* m, const MeshIoContext& io);
  void WireDialRegistry(MeshHost* m, const MeshIoContext& io);
  void WireCircuitHopReach(MeshHost* m, const MeshIoContext& io);

  MeshMediaPlaneDeps deps_;
  std::function<void(const std::string&)> on_relay_chosen_;
  std::unordered_map<std::string, std::vector<std::string>> peer_listen_mas_;

  std::unique_ptr<IMediaRelayClient> media_relay_client_;
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<ICircuitHopReach> circuit_hop_reach_;
  IDialRegistry* test_dial_ = nullptr;
  ICircuitHopReach* test_circuit_reach_ = nullptr;
  PunchIntroducerWalk punch_;
  CircuitRendezvousCoordinator rendezvous_;
  DeferredSelf deferred_;
};

} // namespace pbr
