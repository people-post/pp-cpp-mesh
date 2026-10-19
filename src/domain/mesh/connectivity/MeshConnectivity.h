#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/directory/MeshHopTypes.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/reach/CircuitRendezvousCoordinator.h"
#include "domain/mesh/reach/MeshReachPorts.h"
#include "domain/mesh/reach/PunchIntroducerWalk.h"
#include "common/Lifetime.h"
#include "common/SerialExecutor.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What connectivity needs from the product. Hop candidates are not here: they are product policy
 * (contacts, directory, DHT, seeds) the app computes and pushes (`SetHopPolicy`) — the mesh never
 * calls into the app for them (THREADING.md § Mesh threads, rule 1).
 */
struct MeshConnectivityDeps {
  std::function<MeshHost*()> mesh;
  /** A peer listen addr was registered (LAN discovery bookkeeping). */
  std::function<void(const std::string& peer_id)> note_lan_peer_id;
  /** Without a dial registry: record one direct endpoint (delivery plane). */
  std::function<void(const std::string& key, const std::string& multiaddr)> register_direct_endpoint;
};

/** Candidate policy as the app last pushed it (read on the Amp IO strand). */
struct MeshHopPolicy {
  /** Rendezvous surface (contacts ∪ directory ∪ DHT ∪ seeds); self / target are filtered by the mesh. */
  std::vector<MeshHopCandidate> rendezvous_candidates;
  /** Effective bootstrap seeds (configured ∪ directory). */
  std::vector<MeshHopCandidate> bootstrap_seeds;
  MeshPunchIntroducers punch_introducers;
};

/**
 * This node's mesh, as its consumers need it (local PeerId, listen / advertise addrs, relay and
 * punch availability) — published by the owner while wired, empty otherwise, so consumers on other
 * owners never read the MeshHost the product hub may be tearing down.
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

/**
 * Reaching peers over the mesh, for every product consumer (calls, broadcast, and messaging in
 * time): the dial registry + peer listen book, circuit / service reach, and the reach pieces it is
 * built from — `PunchIntroducerWalk` (punch step) and `CircuitRendezvousCoordinator` (relay surface
 * for dialing, and parking so this node is reachable). Composition and lifecycle only: reach
 * mechanics live in `domain/mesh/reach` and `domain/mesh/reachability`. Media is not here — the
 * media_relay client lives in `MeshMediaRelay`, which builds on this.
 *
 * The owner sequences rewires: dependents holding `Dial()` / `CircuitReach()` must be detached
 * before `Wire`, `ResetDialRegistry`, `ResetAfterMeshStop` or teardown replace them.
 *
 * Threading (THREADING.md § Mesh threads): its state lives on the wired mesh's io lane
 * (`MeshHost::IoLane`), next to the Amp state it drives; unwired, on the caller of an edge.
 * Lifecycle edges (deps, hooks, Wire / Reset / Clear, test path) run there and the caller waits
 * (`RunEdge`: serialized among themselves; callers are the app's threads, never the lane). Listen
 * registrations, local-view refreshes and relay-chosen notices post there (dropped if the mesh they
 * were posted for is gone by then); the listen book, local view and policy are read as published
 * snapshots. The candidate policy is the app's, pushed from any thread (`SetHopPolicy`), never
 * evaluated here. The relay-chosen consumer hook runs on the lane — consumers hop to their own
 * thread. The object accessors (`Dial` / `CircuitReach` / `Lane`) are read by consumers at their
 * bind points, between rewire edges (L015 sequencing). Async callbacks that capture `this` (and the
 * rendezvous coordinator's) are dropped after `InvalidateAsyncOps`.
 */
class MeshConnectivity : public Module {
public:
  using SignalingPunchFn = PunchIntroducerWalk::SignalingPunchFn;

  MeshConnectivity();
  ~MeshConnectivity() override;
  MeshConnectivity(const MeshConnectivity&) = delete;
  MeshConnectivity& operator=(const MeshConnectivity&) = delete;

  void SetDeps(MeshConnectivityDeps deps);
  /** Last-resort punch through a consumer's own signaling when Amp introducers are exhausted (H012). */
  void SetSignalingPunch(SignalingPunchFn punch);
  /**
   * Any thread: the app's candidate policy, computed off the mesh's threads; it replaces the last
   * one. The app pushes again whenever its inputs change (and periodically for those that change
   * without notice); it survives Wire / Reset.
   */
  void SetHopPolicy(MeshHopPolicy policy);
  /** Any thread: the policy the IO side sees. */
  std::shared_ptr<const MeshHopPolicy> HopPolicy() const;
  /**
   * Recompute the local view on the owner now (reachability changed the advertised addrs). Also
   * refreshed at Wire and every few seconds while wired.
   */
  void RefreshLocalView();
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

  /** (Re)create the dial registry and circuit reach from the running mesh; arm re-park. */
  void Wire();
  /** Tests / harness without MeshHost objects: use these instead (not owned), reach running on `lane`. */
  void BindTestPath(IDialRegistry* dial, ICircuitHopReach* circuit_reach,
                    std::shared_ptr<pp::SerialExecutor> lane = nullptr);
  /** Drop async callbacks, relay-chosen notices and the re-park listener (mesh stop / teardown). */
  void InvalidateAsyncOps();
  /** Dial registry (capability refresh); Wire makes a new one. */
  void ResetDialRegistry();
  /** After the mesh stopped: dial registry and circuit reach (they referenced its links / tunnel). */
  void ResetAfterMeshStop();
  /** Everything, including the listen book (owner teardown). */
  void Clear();

  IDialRegistry* Dial() const;
  ICircuitHopReach* CircuitReach() const;
  /** The wired mesh's `work` executor (null when not wired: inline), for objects built on this. */
  std::shared_ptr<pp::Executor> WorkExecutor() const { return work_; }
  /** The wired mesh's io lane, where reach runs (`PeerReachPorts`); null when not wired. */
  std::shared_ptr<pp::SerialExecutor> Lane() const { return test_lane_ ? test_lane_ : StateLane(); }
  /**
   * A lifecycle edge of an object built on this one (`MeshMediaRelay`): runs where this one's state
   * lives — the wired mesh's io lane, else the caller — while the caller waits. Never from the lane.
   */
  void RunEdge(const std::function<void()>& task);
  /** The mesh it wires from (null when none); for objects built on this (`MeshMediaRelay`). */
  MeshHost* Mesh() const { return mesh(); }

  /**
   * Liveness of the objects handed out to consumers' async work (dial registry, circuit reach, and
   * `MeshMediaRelay`'s relay client): bumped on the owner before any of them is replaced or freed.
   */
  pp::Lifetime::Token ObjectsToken() const { return objects_.token(); }
  uint64_t ObjectsSnapshot() const { return objects_.Snapshot(); }
  /** Owner: one of the handed-out objects is about to be replaced (`MeshMediaRelay`'s relay client). */
  void InvalidateObjects() { objects_.Invalidate(); }

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
  std::shared_ptr<pp::SerialExecutor> StateLane() const;
  void SetStateLane(std::shared_ptr<pp::SerialExecutor> lane);
  /** Debug assertions: on the state lane (unwired: anywhere, an edge holds the edge lock). */
  bool OnStateLane() const;
  /** `task` where the state lives: posted to the lane; unwired, now on the caller (as an edge). */
  void Post(std::function<void()> task);
  std::string RegisterPeerListenMultiaddrsOnLane(const std::string& key, const std::vector<std::string>& multiaddrs);
  void RefreshLocalViewOnLane();
  void PublishLocalView(MeshLocalView view);
  /** Refresh the local view periodically while wired (the mesh's addrs change without notice). */
  void ArmLocalViewRefresh();
  void PublishListenBook();
  void WireDialRegistry(MeshHost* m, const MeshIoContext& io);
  void WireCircuitHopReach(MeshHost* m, const MeshIoContext& io);

  MeshConnectivityDeps deps_;
  std::function<void(const std::string&)> on_relay_chosen_;
  ListenBook peer_listen_mas_;  // state lane
  mutable std::mutex listen_book_mu_;
  std::shared_ptr<const ListenBook> listen_book_ = std::make_shared<const ListenBook>();
  mutable std::mutex hop_policy_mu_;
  std::shared_ptr<const MeshHopPolicy> hop_policy_ = std::make_shared<const MeshHopPolicy>();
  uint64_t local_view_timer_ = 0;  // state lane
  bool wired_ = false;             // state lane: between Wire and InvalidateAsyncOps
  /** Serializes edges (they nest: Clear runs InvalidateAsyncOps). Never taken on the lane. */
  std::recursive_mutex edge_mu_;
  /** Guards `lane_` only: a leaf, never held while running anything. */
  mutable std::mutex lane_mu_;
  /** The wired mesh's io lane: where the state lives (null: unwired). */
  std::shared_ptr<pp::SerialExecutor> lane_;
  mutable std::mutex local_view_mu_;
  std::shared_ptr<const MeshLocalView> local_view_ = std::make_shared<const MeshLocalView>();
  std::unique_ptr<PeerSessionDialRegistry> dial_registry_;
  std::unique_ptr<ICircuitHopReach> circuit_hop_reach_;
  std::shared_ptr<pp::Executor> work_;  // set at Wire, read at consumers' bind points like Dial()
  std::shared_ptr<pp::SerialExecutor> test_lane_;
  IDialRegistry* test_dial_ = nullptr;
  ICircuitHopReach* test_circuit_reach_ = nullptr;
  PunchIntroducerWalk punch_;
  CircuitRendezvousCoordinator rendezvous_;
  pp::Lifetime deferred_;
  pp::Lifetime objects_;
  /** Everything Post()ed: the destructor drops what is queued and waits for one running. */
  pp::Lifetime self_;
};

} // namespace pbr
