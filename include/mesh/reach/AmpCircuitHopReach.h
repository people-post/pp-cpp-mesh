#pragma once

#include "common/Lifetime.h"
#include "mesh/host/MeshPorts.h"
#include "mesh/l4/circuit/CircuitRelayRefusals.h"
#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/reach/MeshReachPorts.h"
#include "mesh/reach/PunchIntroducerWalk.h"
#include "mesh/types/ServiceLoad.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pp::mesh {

/**
 * ICircuitHopReach over Amp CircuitClientCoordinator + AmpCircuitHopRegistry ([A020]).
 * Reach for relay hops and peers (NAT) when Amp is the mesh transport entry.
 *
 * Prefer TryEnsure*Async. Product Wire passes empty io_pump + post_io when MeshPump runs
 * (MeshPump owns Drive; nested Tick from Coordinator races PeerLink teardown). Harnesses
 * without MeshPump may supply io_pump for flush-between-relay attempts.
 */
class AmpCircuitHopReach final : public ICircuitHopReach {
public:
  using IoPump = std::function<void()>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;
  using CollectRelays = std::function<std::vector<std::string>(const std::string& exclude_peer_id)>;
  /** Optional L3.25b: Amp punch before circuit (H002). */
  using TryPunchAsync = std::function<void(const std::string& target_peer_id, PunchIntroducerWalk::Done on_done)>;
  /** Optional L3.25c: punch via an explicit introducer (circuit R1). */
  using TryPunchViaIntroducerAsync = std::function<void(
      const std::string& introducer_peer_key, const std::string& target_peer_id, PunchIntroducerWalk::Done on_done)>;

  /** One bridge's failure, as reach's (the tunnel's chain in `message`). */
  static CircuitReachFailure WrapTunnelFailure(const CircuitClientCoordinator::Failure& tunnel);
  /** The punch step's failure, as reach's. */
  static CircuitReachFailure WrapPunchFailure(const PunchIntroducerWalk::Failure& punch);

  AmpCircuitHopReach(CircuitClientCoordinator& circuit, AmpCircuitHopRegistry& hops, IChatPeerLinks& links,
                     IoPump io_pump, CollectRelays collect_relays, TryPunchAsync try_punch = {},
                     TryPunchViaIntroducerAsync try_punch_via_introducer = {}, IoPost post_io = {},
                     IoAfter post_after = {});

  void TryEnsureHopReachableAsync(const std::string& hop_peer_id, CircuitReachDone on_done) override;
  void TryEnsurePeerReachableViaAsync(const std::string& peer_key, CircuitReachDone on_done, bool allow_circuit,
                                      std::vector<std::string> advertised_relays) override;
  void TryEnsurePeerReachableAsync(const std::string& peer_key, CircuitReachDone on_done,
                                   bool allow_circuit = true) override;
  void TryEnsureCircuitOnlyAsync(const std::string& peer_key, CircuitReachDone on_done,
                                 std::vector<std::string> advertised_relays) override;
  void TryUpgradeToDirectAsync(const std::string& peer_key, CircuitReachDone on_done) override;
  void TryEnsureRelayedAsync(const std::string& peer_key, CircuitReachDone on_done,
                             CircuitStandbyPriority standby_priority = CircuitStandbyPriority::None) override;

  /**
   * What relays said of their own load (MeshHost's PeerLoadBook): a relay that
   * said busy is tried after open ones, a full or draining one last
   * (docs/PEER_REACH.md § Service load). Null: the order without it.
   */
  void SetPeerLoads(const PeerLoadBook* book) { peer_loads_ = book; }

  /**
   * The clock a circuit's envelope is measured on, in ms (its timers run on post_after: the Amp clock).
   * None: steady_clock. A bench on a VirtualClock passes that clock.
   */
  void SetClock(std::function<int64_t()> now_ms) { now_ms_ = std::move(now_ms); }

  /** Its mesh is going away (MeshHost::IsStopping): the synchronous waits below are refused. */
  void SetStopping(std::function<bool()> stopping) { stopping_ = std::move(stopping); }

  CircuitReachRoe TryEnsureHopReachable(const std::string& hop_peer_id) override;
  CircuitReachRoe TryEnsurePeerReachable(const std::string& peer_key) override;
  void AbortPending() override;
  std::string LastGoodRelayPeerKey() const override { return last_good_relay_peer_key_; }

  /**
   * H011 L3.1c: invoked on Amp IO after a successful bridge Install (chosen R1).
   * CircuitRendezvousCoordinator uses this (via MeshConnectivity) for sticky cache / optional late-reserve announce path.
   */
  using OnRelayChosen = std::function<void(const std::string& relay_peer_key)>;
  void SetOnRelayChosen(OnRelayChosen cb) { on_relay_chosen_ = std::move(cb); }

private:
  /** A link to `target_peer_id` nested through a circuit relay's carrier (A024): a peer's, or a hop's. */
  void EnsureViaCircuitAsync(const std::string& target_peer_id, CircuitReachDone on_done,
                             CircuitStandbyPriority standby_priority = CircuitStandbyPriority::None,
                             std::vector<std::string> advertised_relays = {});

  /** One EnsureViaCircuit's relays and bridges, and one bridge of it (in the .cpp). */
  struct BridgeWalk;
  struct WalkBridge;
  using WalkPtr = std::shared_ptr<BridgeWalk>;
  using BridgePtr = std::shared_ptr<WalkBridge>;
  /** Why a walk starts no more bridges. */
  enum class WalkStop : uint8_t { None, Exhausted, Cap, Envelope };
  /** Start bridges, best relay first, until kCircuitParallelBridges are out — or end the walk. */
  void FillBridges(const WalkPtr& walk);
  void FinishWalkOutOfRelays(const WalkPtr& walk, WalkStop stop, int64_t remaining_ms);
  /** Not the target, has an endpoint, a dialable Preferred (else the walk's miss says why). */
  bool RelayUsable(BridgeWalk& walk, const std::string& relay_key);
  void LaunchBridge(const WalkPtr& walk, const std::string& relay_key, int64_t bridge_timeout_ms, int64_t remaining_ms);
  void ForgetBridge(BridgeWalk& walk, const WalkBridge& bridge);
  void RefillSoon(const WalkPtr& walk);
  void OnBridgeFinished(const WalkPtr& walk, const BridgePtr& bridge, CircuitClientCoordinator::BridgeRoe result);
  void OnBridgeMissed(const WalkPtr& walk, const WalkBridge& bridge, const CircuitClientCoordinator::BridgeRoe& result);
  void OnBridgeTimeout(const WalkPtr& walk, const BridgePtr& bridge);
  void NestOverBridge(const WalkPtr& walk, const BridgePtr& bridge,
                      const std::shared_ptr<pp::amp::ChannelSession>& session);
  void OnNestedDone(const WalkPtr& walk, const WalkBridge& bridge,
                    const std::shared_ptr<pp::amp::ChannelSession>& session, IChatPeerLinks::LinkRoe nested,
                    bool timed_out);
  void NoteRelayChosen(const std::string& relay_key);
  /** Drop a bridge the walk will not use: close its session when it acked, cancel its tunnel. */
  void ReleaseBridge(CircuitTunnelId id, const std::shared_ptr<pp::amp::ChannelSession>& session);
  /** The walk's answer, once; the bridges still out are cancelled. */
  void FinishWalk(const WalkPtr& walk, CircuitReachRoe result);
  int64_t NowMs() const;
  /** `on_timeout` once `delay_ms` passed (Amp's clock when post_after is wired), unless `settled`. */
  void AfterMs(int64_t delay_ms, const std::shared_ptr<std::atomic<bool>>& settled, std::function<void()> on_timeout);

  void NoteInflightTunnel(CircuitTunnelId id);
  void ClearInflightTunnel(CircuitTunnelId id);
  std::vector<CircuitTunnelId> TakeInflightTunnels();

  CircuitClientCoordinator& circuit_;
  AmpCircuitHopRegistry& hops_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  IoPost post_io_;
  IoAfter post_after_;
  CollectRelays collect_relays_;
  TryPunchAsync try_punch_;
  TryPunchViaIntroducerAsync try_punch_via_introducer_;
  OnRelayChosen on_relay_chosen_;
  /** AbortPending Invalidates — in-flight EnsureViaCircuit / punch cbs no-op. */
  std::function<bool()> stopping_;
  const PeerLoadBook* peer_loads_ = nullptr;
  pp::Lifetime deferred_;
  /** StartBridge ids in flight (two at once per walk, a nest's until it settles); AbortPending cancels them. */
  std::mutex inflight_mu_;
  std::vector<uint64_t> inflight_tunnels_; // inflight_mu_
  std::function<int64_t()> now_ms_;
  /** Last relay that completed a bridge Install (sticky first try — H010). */
  std::string last_good_relay_peer_key_;
  /** Relays that refused a circuit lately: tried after the others. */
  CircuitRelayRefusals refusals_;
};

} // namespace pp::mesh
