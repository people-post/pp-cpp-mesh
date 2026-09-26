#pragma once

#include "feature/calls/CallTopologyRelayDeps.h"

#include "common/Module.h"

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

/** Kind of mesh link a reach settled on. */
enum class PeerLinkKind : uint8_t {
  Unknown = 0,
  /** ADP association from a plain dial. */
  Direct,
  /** ADP association opened by a punch. */
  Punched,
  /** Nested link over a relay circuit carrier (or a carrier-only link). */
  Relayed,
};

const char* PeerLinkKindName(PeerLinkKind kind);

/**
 * How this side takes part in establishing the link.
 *
 * - **Reach**: this side wants the link — dial, then build a relay circuit, then punch.
 * - **Await**: the peer is reaching this side (an out-of-band agreement says so). Keep the relay
 *   link that makes us reachable intact (no speculative private dials), punch toward the peer, no
 *   circuit of our own, and wait longer for the peer's link to land.
 *
 * Await is only valid when the peer is known to Reach; this class does not negotiate roles.
 */
enum class PeerReachMode : uint8_t {
  Reach = 0,
  Await,
};

struct PeerReachRequest {
  /**
   * Dial keys the mesh may know this peer under; `keys[0]` is primary (dial / circuit target).
   * Extra keys are aliases that only count toward "already connected / dialable".
   */
  std::vector<std::string> keys;
  PeerReachMode mode = PeerReachMode::Reach;
  /** Previous direct link was unusable (e.g. one-way media) — insist on a relayed link. */
  bool exclude_direct = false;
  /** Previous "connected" link was stale — drop it and redial (B39). */
  bool fresh_link = false;
};

struct PeerReachResult {
  PeerLinkKind kind = PeerLinkKind::Unknown;
  /** Settled immediately on a link that was already Connected (no dial this time). */
  bool reused_link = false;
};

using PeerReachId = uint64_t;

/**
 * Establishes a mesh link to one peer: direct dial (ADP EnsureAssociation), relay circuit and
 * punch (ICircuitHopReach), gated on a Connected bootstrap seed park. Knows nothing about what
 * the link will carry.
 *
 * Threading: all reach state lives on the Coordinator strand. `on_done` runs at most once — on
 * the Coordinator, or inline on the thread that calls Cancel / CancelAll. Destruction drops
 * pending callbacks without running them.
 */
class PeerReachCoordinator : public Module {
public:
  using SeedParkAwait = std::function<void(std::function<void(bool parked)>, int timeout_ms)>;
  using Done = std::function<void(Roe<PeerReachResult>)>;

  PeerReachCoordinator(IDialRegistry* dial, ICircuitHopReach* circuit);
  ~PeerReachCoordinator() override;

  PeerReachCoordinator(const PeerReachCoordinator&) = delete;
  PeerReachCoordinator& operator=(const PeerReachCoordinator&) = delete;

  /** Rewire mesh ports (listen sync). In-flight reaches pick up the new pointers on their next step. */
  void SetDeps(IDialRegistry* dial, ICircuitHopReach* circuit);
  /** Await at least one bootstrap seed Connected before circuit / punch (H010). */
  void SetSeedParkAwait(SeedParkAwait park);
  /** Shrink the direct-dial budget for gtests (<= 0 = production default). */
  void SetDialBudgetMsForTest(int budget_ms);

  PeerReachId Ensure(PeerReachRequest request, Done on_done);
  /**
   * Abort any in-flight dial to `key` and clear its backoff, so the channel open (or the next
   * reach) starts from a clean dial state. Does not touch a Connected link.
   */
  void AbandonDial(const std::string& key);

  // --- Link-state operations for owners. UI thread (as the owners' callers were before). ---

  /** Mesh ports are bound (a dial registry exists). */
  bool Available() const;
  /** A relay circuit path is available (circuit / punch reach wired). */
  bool HasCircuitReach() const;
  /** A relay circuit hop is registered for `key` (the link rides a relay). */
  bool HasRelayHop(const std::string& key) const;
  /**
   * Forget the path state for `key` — relay hop registration and dial backoff — so the next
   * reach re-selects a path from scratch (e.g. after one-way media on the current one).
   */
  void ForgetPath(const std::string& key);
  /** The owner no longer needs a link to `key`: abort its in-flight dial, drop its relay hop. */
  void ReleasePeer(const std::string& key);
  /**
   * Abort in-flight circuit / punch attempts (StartBridge etc.). Circuit reach is shared, so this
   * aborts every pending attempt, not just this owner's — callers use it on failure / teardown.
   */
  void AbortCircuitAttempts();
  /**
   * Pick the dial key for a peer known under both an alias (e.g. `account:`) and its mesh PeerId.
   * The PeerId is preferred: the Connected PeerLink lives under it, while the alias can look
   * dialable through a stale entry (dogfood 7bd62: AssociationNotReady forever). When only the
   * alias is dialable, its Preferred multiaddr is copied onto the PeerId. Returns the alias only
   * when the alias is dialable and the PeerId still is not (no Preferred to copy).
   */
  std::string PreferDialKey(const std::string& alias, const std::string& peer_id);
  /** Completes the reach with an error (inline) and drops its pending steps. */
  void Cancel(PeerReachId id);
  void CancelAll();

private:
  struct Attempt;
  using AttemptPtr = std::shared_ptr<Attempt>;

  void Start(const AttemptPtr& a);
  void Tick(const AttemptPtr& a);
  void ScheduleTick(const AttemptPtr& a, int delay_ms);
  void Finish(const AttemptPtr& a, Roe<PeerReachResult> result);
  bool TrySettleConnected(const AttemptPtr& a);
  bool MaybeParkBeforePrivateDial(const AttemptPtr& a);
  bool MaybeStartAssociation(const AttemptPtr& a);
  void OnAssociationDone(const AttemptPtr& a, Roe<void> assoc, bool connected_now, bool direct_now);
  bool MaybeStartCircuit(const AttemptPtr& a);
  void KickCircuit(const AttemptPtr& a, bool allow_circuit);
  void OnCircuitDone(const AttemptPtr& a, Roe<void> via, bool connected_now, bool relayed_now);

  bool AnyConnected(const Attempt& a) const;
  bool AnyConnectedDirect(const Attempt& a) const;
  bool AnyDialable(const Attempt& a) const;
  bool AnyCircuitHop(const Attempt& a) const;
  bool PreferredIsPublic(const Attempt& a) const;
  void ClearBackoff(const Attempt& a);

  std::atomic<IDialRegistry*> dial_{nullptr};
  std::atomic<ICircuitHopReach*> circuit_{nullptr};
  mutable std::mutex mu_;
  SeedParkAwait seed_park_;
  int64_t dial_budget_ms_;
  PeerReachId next_id_ = 1;
  std::unordered_map<PeerReachId, AttemptPtr> attempts_;
  /** Guards `this` in deferred steps; bumped on destruction. */
  std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace pbr
