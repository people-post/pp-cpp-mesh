#pragma once

#include "base/p2p/AmpCircuitHopRegistry.h"
#include "base/p2p/CircuitTunnelCoordinator.h"
#include "base/mesh/link/PeerLinkManager.h"
#include "feature/messaging/CallTopologyRelayDeps.h"

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

  AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops,
                     amp::PeerLinkManager& links, IoPump io_pump, CollectRelays collect_relays);

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override;
  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override;

private:
  Roe<void> EnsureViaCircuit(const std::string& target_peer_id, const std::string& target_protocol,
                             bool register_endpoint, bool nested_session);

  CircuitTunnelCoordinator& circuit_;
  AmpCircuitHopRegistry& hops_;
  amp::PeerLinkManager& links_;
  IoPump io_pump_;
  CollectRelays collect_relays_;
};

} // namespace pbr
