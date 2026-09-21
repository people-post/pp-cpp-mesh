#pragma once

#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/host/MeshPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * ICircuitHopReach over Amp CircuitTunnelCoordinator + AmpCircuitHopRegistry ([A020]).
 * SoftMigrate / call-media NAT when Amp is the mesh transport entry.
 *
 * Prefer TryEnsure*Async. Product Wire passes empty io_pump + post_io when MeshPump runs
 * (MeshPump owns Drive; nested Tick from Coordinator races PeerLink teardown). Harnesses
 * without MeshPump may supply io_pump for flush-between-relay attempts.
 */
class AmpCircuitHopReach final : public ICircuitHopReach {
public:
  using IoPump = std::function<void()>;
  using IoPost = std::function<void(std::function<void()>)>;
  using CollectRelays = std::function<std::vector<std::string>(const std::string& exclude_peer_id)>;
  /** Optional L3.25b: Amp punch before circuit (H002). */
  using TryPunchAsync =
      std::function<void(const std::string& target_peer_id, std::function<void(Roe<void>)> on_done)>;
  /** Optional L3.25c: punch via an explicit introducer (circuit R1). */
  using TryPunchViaIntroducerAsync =
      std::function<void(const std::string& introducer_peer_key, const std::string& target_peer_id,
                         std::function<void(Roe<void>)> on_done)>;

  AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops, IChatPeerLinks& links,
                     IoPump io_pump, CollectRelays collect_relays, TryPunchAsync try_punch = {},
                     TryPunchViaIntroducerAsync try_punch_via_introducer = {}, IoPost post_io = {});

  void TryEnsureHopReachableAsync(const std::string& hop_peer_id,
                                  std::function<void(Roe<void>)> on_done) override;
  void TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                        std::function<void(Roe<void>)> on_done,
                                        bool allow_circuit = true) override;
  void TryUpgradeToDirectAsync(const std::string& peer_key,
                               std::function<void(Roe<void>)> on_done) override;

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override;
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override;
  Roe<void> TryUpgradeToDirect(const std::string& peer_key) override;
  void AbortPending() override;

private:
  void EnsureViaCircuitAsync(const std::string& target_peer_id, const std::string& target_protocol,
                             bool register_endpoint, bool nested_session,
                             std::function<void(Roe<void>)> on_done);

  void NoteInflightTunnel(CircuitTunnelId id);
  void ClearInflightTunnel(CircuitTunnelId id);
  CircuitTunnelId TakeInflightTunnel();

  CircuitTunnelCoordinator& circuit_;
  AmpCircuitHopRegistry& hops_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  IoPost post_io_;
  CollectRelays collect_relays_;
  TryPunchAsync try_punch_;
  TryPunchViaIntroducerAsync try_punch_via_introducer_;
  std::atomic<uint64_t> abort_gen_{0};
  /** Active StartBridge id for this reach chain; AbortPending CancelTunnel's it (hard cancel). */
  std::atomic<uint64_t> inflight_tunnel_value_{0};
  /** Last relay that completed a bridge Install (sticky first try — H010). */
  std::string last_good_relay_peer_key_;

  Roe<void> DemoteCircuitHop(const std::string& peer_key, const std::string& target_protocol,
                             CircuitTunnelId tunnel_id);
};

} // namespace pbr
