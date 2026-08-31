#include "feature/messaging/AmpCircuitHopReach.h"

#include "base/p2p/CallMediaDirectService.h"
#include "base/p2p/MediaRelayService.h"
#include "base/p2p/SettledWait.h"

#include <chrono>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

AmpCircuitHopReach::AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops,
                                       amp::PeerLinkManager& links, IoPump io_pump,
                                       CollectRelays collect_relays)
    : circuit_(circuit), hops_(hops), links_(links), io_pump_(std::move(io_pump)),
      collect_relays_(std::move(collect_relays)) {}

Roe<void> AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  return EnsureViaCircuit(hop_peer_id, kMediaRelayProtocolId);
}

Roe<void> AmpCircuitHopReach::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (peer_key.empty()) {
    return Error("missing call peer");
  }
  if (links_.GetLinkSnapshot(peer_key).has_endpoint || hops_.HasAny(peer_key)) {
    return {};
  }
  return EnsureViaCircuit(peer_key, kCallMediaDirectProtocolId);
}

Roe<void> AmpCircuitHopReach::EnsureViaCircuit(const std::string& target_peer_id,
                                               const std::string& target_protocol) {
  if (!circuit_.IsStarted()) {
    return Error("amp circuit-relay not available");
  }
  if (target_peer_id.empty() || target_protocol.empty()) {
    return Error("amp circuit hop incomplete");
  }
  if (hops_.Find(target_peer_id, target_protocol)) {
    return {};
  }
  if (links_.GetLinkSnapshot(target_peer_id).has_endpoint) {
    return {};
  }
  if (!collect_relays_) {
    return Error("no dialable circuit relays");
  }
  const std::vector<std::string> relays = collect_relays_(target_peer_id);
  if (relays.empty()) {
    return Error("no dialable circuit relays");
  }

  CircuitBridgeTarget bridge_target;
  bridge_target.target_peer_id = target_peer_id;
  bridge_target.target_protocol = target_protocol;
  if (auto ma = links_.PreferredMultiaddr(target_peer_id)) {
    bridge_target.target_multiaddr = *ma;
  }

  for (const std::string& relay_key : relays) {
    if (relay_key == target_peer_id || !links_.GetLinkSnapshot(relay_key).has_endpoint) {
      continue;
    }

    SettledWait<CircuitTunnelBridgeResult> wait;
    const CircuitTunnelId id = circuit_.StartBridge(
        relay_key, bridge_target, {}, {},
        [wait](Roe<CircuitTunnelBridgeResult> result) { wait.Finish(std::move(result)); }, 8000);
    if (!id) {
      continue;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(10000);
    while (Clock::now() < deadline && !wait.IsSettled()) {
      if (io_pump_) {
        io_pump_();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    auto bridged = wait.Wait(std::chrono::milliseconds(10000), Error("amp circuit bridge timed out"));
    if (!bridged || !bridged->ok || !bridged->session) {
      continue;
    }
    if (!bridged->resolved_multiaddr.empty()) {
      (void)links_.RegisterEndpoint(target_peer_id, bridged->resolved_multiaddr);
    }
    auto installed =
        hops_.Install(target_peer_id, relay_key, target_protocol, bridged->session, id);
    if (!installed) {
      return installed;
    }
    return {};
  }
  return Error("circuit hop reach failed");
}

} // namespace pbr
