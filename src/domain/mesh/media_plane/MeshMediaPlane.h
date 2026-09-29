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

#include <atomic>
#include <mutex>
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
 * seeds) the wiring feature computes — `domain/mesh` does not read contacts. The three policy
 * providers are evaluated on the Connectivity owner (at Wire, every few seconds, and on
 * `RefreshHopPolicy`); the Amp IO side only reads the resulting `MeshHopPolicy` snapshot.
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
 * Threading (thread-ownership t3-2): the plane lives on the Connectivity owner (`pp-connectivity`).
 * Lifecycle edges (deps, hooks, Wire / Reset / Clear, test path) run there and the caller waits
 * (`AppRuntime::RunAndWait` — callers are UI or the media-sessions owner, never below it). Listen
 * registrations post there; the listen book is read as a published snapshot. Circuit reach's
 * relay-chosen notice hops from Amp IO to the owner, and the consumer hook runs there — consumers
 * hop to their own owner. The object accessors (`RelayClient` / `Dial` / `CircuitReach`) are read by
 * consumers at their bind points, between the owner's rewire edges (L015 sequencing). Async
 * callbacks that capture `this` (and the rendezvous coordinator's) are dropped after
 * `InvalidateAsyncOps`.
 */
/** Candidate policy as of the owner's last refresh (read on the Amp IO strand). */
struct MeshHopPolicy {
  std::vector<MeshHopCandidate> rendezvous_candidates;
  std::vector<MeshHopCandidate> bootstrap_seeds;
  MeshPunchIntroducers punch_introducers;
};

/**
 * This node's mesh, as the media consumers need it (local PeerId, listen / advertise addrs, relay
 * and punch availability) — published by the owner while wired, empty otherwise, so consumers on
 * other owners never read the MeshHost the product hub may be tearing down.
 */
struct MeshLocalView {
  bool amp_up = false;
  std::string local_peer_id;
  std::string amp_listen_multiaddr;
  std::vector<std::string> advertised_listen_multiaddrs;
  bool media_relay_started = false;
  bool punch_started = false;
  std::vector<std::string> punch_candidate_addrs;
};

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
  /**
   * Recompute the candidate policy and the local view on the owner now (contacts / directory /
   * seeds / reachability changed). They are also refreshed at Wire and every few seconds.
   */
  void RefreshHopPolicy();
  /** Any thread: the policy the IO side sees. */
  std::shared_ptr<const MeshHopPolicy> HopPolicy() const;
  /** Any thread: this node's mesh as of the owner's last refresh (empty when not wired). */
  std::shared_ptr<const MeshLocalView> LocalView() const;
  /**
   * H012 signaling punch: burst toward `peer_addrs` for `window_ms`. Runs on the owner (reads the
   * mesh there, only while wired); `on_done` on whichever thread the punch completes.
   */
  void SignalingPunchBurstAsync(std::vector<std::string> peer_addrs, int window_ms,
                                std::function<void(Roe<void>)> on_done);

  /** Circuit reach chose a rendezvous relay (H011: calls announce it to the call peer). */
  void SetOnRelayChosen(std::function<void(const std::string& relay_peer_id)> callback);

  /** (Re)create relay client, dial registry and circuit reach from the running mesh; arm re-park. */
  void Wire();
  /**
   * Tests / harness without MeshHost objects: use these instead (not owned). `relay` stands in for
   * the Amp media_relay client (group-call compose); null keeps the wired one.
   */
  void BindTestPath(IDialRegistry* dial, ICircuitHopReach* circuit_reach, IMediaRelayClient* relay = nullptr);
  /** Drop async callbacks, relay-chosen notices and the re-park listener (mesh stop / teardown). */
  void InvalidateAsyncOps();
  void ResetRelayClient();
  /** Relay client + dial registry (capability refresh). */
  void ResetRelayClients();
  /** After the mesh stopped: dial registry and circuit reach (they referenced its links / tunnel). */
  void ResetAfterMeshStop();
  /** Everything, including the listen book (owner teardown). */
  void Clear();

  IMediaRelayClient* RelayClient() const { return test_relay_ ? test_relay_ : media_relay_client_.get(); }
  IDialRegistry* Dial() const;
  ICircuitHopReach* CircuitReach() const;
  /** media_relay client + dial + service reach, for `AttachToMediaRelayAsync` users. */
  MediaRelayAttachPorts RelayAttachPorts() const;
  /** True when the mesh runs a started Amp media_relay coordinator. */
  bool AmpRelayAvailable() const;

  // --- peer listen book ---------------------------------------------------------------------
  /**
   * Rank, merge and register a peer's listen multiaddrs under `key` (account id or PeerId), on the
   * owner. `on_registered` (on the owner) gets the PeerId of the best dialable addr (empty when none
   * was dialable).
   */
  void RegisterPeerListenMultiaddrs(const std::string& key, const std::vector<std::string>& multiaddrs,
                                    std::function<void(const std::string& peer_id)> on_registered = {});
  using ListenBook = std::unordered_map<std::string, std::vector<std::string>>;
  /** Any thread: the book as of the owner's last registration. */
  std::shared_ptr<const ListenBook> PeerListenBook() const;
  /** LAN-confirmed: a connected Amp link (mDNS / LAN dial). */
  bool PeerLanConfirmed(const std::string& peer_id) const;

  // --- reach --------------------------------------------------------------------------------
  Roe<void> TryEnsureCircuitHopReachable(const std::string& hop_peer_id);
  Roe<void> TryEnsurePeerReachable(const std::string& peer_key);
  void TryEnsurePeerReachableAsync(const std::string& peer_key, std::function<void(Roe<void>)> on_done);

  /** Rendezvous relays: dial surface for reach, parking so this node is reachable (stable object). */
  CircuitRendezvousCoordinator& Rendezvous() { return rendezvous_; }

private:
  MeshHost* mesh() const { return deps_.mesh ? deps_.mesh() : nullptr; }
  std::string RegisterPeerListenMultiaddrsOnOwner(const std::string& key, const std::vector<std::string>& multiaddrs);
  void RefreshHopPolicyOnOwner();
  void PublishLocalView(MeshLocalView view);
  /** Re-evaluate the policy periodically while wired (the inputs change without notice). */
  void ArmHopPolicyRefresh();
  void PublishListenBook();
  void WireMediaRelayClient(MeshHost* m, const MeshIoContext& io);
  void WireDialRegistry(MeshHost* m, const MeshIoContext& io);
  void WireCircuitHopReach(MeshHost* m, const MeshIoContext& io);

  MeshMediaPlaneDeps deps_;
  std::function<void(const std::string&)> on_relay_chosen_;
  ListenBook peer_listen_mas_;  // owner
  mutable std::mutex listen_book_mu_;
  std::shared_ptr<const ListenBook> listen_book_ = std::make_shared<const ListenBook>();
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  mutable std::mutex hop_policy_mu_;
  std::shared_ptr<const MeshHopPolicy> hop_policy_ = std::make_shared<const MeshHopPolicy>();
  uint64_t hop_policy_timer_ = 0;  // owner
  bool wired_ = false;             // owner: between Wire and InvalidateAsyncOps
  mutable std::mutex local_view_mu_;
  std::shared_ptr<const MeshLocalView> local_view_ = std::make_shared<const MeshLocalView>();

  std::unique_ptr<IMediaRelayClient> media_relay_client_;
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<ICircuitHopReach> circuit_hop_reach_;
  IDialRegistry* test_dial_ = nullptr;
  ICircuitHopReach* test_circuit_reach_ = nullptr;
  IMediaRelayClient* test_relay_ = nullptr;
  PunchIntroducerWalk punch_;
  CircuitRendezvousCoordinator rendezvous_;
  DeferredSelf deferred_;
  /** Liveness of relay client / dial / circuit reach handed out in `RelayAttachPorts` (bumped before any is freed). */
  DeferredSelf objects_;
};

} // namespace pbr
