#pragma once

#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/host/MeshPorts.h"
#include "feature/messaging/calls/CallTopologyRelayDeps.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * ICircuitHopReach over Amp CircuitTunnelCoordinator + AmpCircuitHopRegistry ([A020]).
 * SoftMigrate / call-media NAT when Amp is the mesh transport entry.
 */
class AmpCircuitHopReach final : public ICircuitHopReach {
public:
  using IoPump = std::function<void()>;
  using CollectRelays = std::function<std::vector<std::string>(const std::string& exclude_peer_id)>;

  AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops, IChatPeerLinks& links,
                     IoPump io_pump, CollectRelays collect_relays);

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override;
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override;

private:
  Roe<void> EnsureViaCircuit(const std::string& target_peer_id, const std::string& target_protocol,
                             bool register_endpoint, bool nested_session);

  CircuitTunnelCoordinator& circuit_;
  AmpCircuitHopRegistry& hops_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  CollectRelays collect_relays_;
};

} // namespace pbr
