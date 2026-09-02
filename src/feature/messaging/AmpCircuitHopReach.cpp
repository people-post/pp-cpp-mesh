#include "feature/messaging/AmpCircuitHopReach.h"

#include "amp/link/PeerLink.h"
#include "base/mesh/l4/call_media/ICallMediaTransport.h"
#include "base/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/SettledWait.h"

#include <chrono>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

AmpCircuitHopReach::AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops,
                                       IChatPeerLinks& links, IoPump io_pump,
                                       CollectRelays collect_relays)
    : circuit_(circuit), hops_(hops), links_(links), io_pump_(std::move(io_pump)),
      collect_relays_(std::move(collect_relays)) {}

Roe<void> AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  return EnsureViaCircuit(hop_peer_id, kMediaRelayProtocolId, /*register_endpoint=*/true,
                          /*nested_session=*/false);
}

Roe<void> AmpCircuitHopReach::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (peer_key.empty()) {
    return Error("missing call peer");
  }
  // Direct ADP or already-connected nested/direct link — no circuit needed.
  if (links_.IsConnected(peer_key) || links_.GetLinkSnapshot(peer_key).has_endpoint) {
    return {};
  }
  // Protocol-specific: a media-relay hop must not short-circuit call-media reach.
  if (hops_.Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId) && links_.IsConnected(peer_key)) {
    return {};
  }
  return EnsureViaCircuit(peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
                          /*nested_session=*/true);
}

Roe<void> AmpCircuitHopReach::EnsureViaCircuit(const std::string& target_peer_id,
                                               const std::string& target_protocol,
                                               const bool register_endpoint, const bool nested_session) {
  if (!circuit_.IsStarted()) {
    return Error("amp circuit-relay not available");
  }
  if (target_peer_id.empty() || target_protocol.empty()) {
    return Error("amp circuit hop incomplete");
  }
  if (!nested_session && hops_.Find(target_peer_id, target_protocol)) {
    return {};
  }
  if (nested_session && links_.IsConnected(target_peer_id)) {
    return {};
  }
  if (!nested_session && links_.GetLinkSnapshot(target_peer_id).has_endpoint) {
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

    if (nested_session) {
      SettledWait<void> nested_wait;
      links_.EstablishNestedOverCarrier(
          target_peer_id, bridged->session, true,
          [nested_wait](IChatPeerLinks::LinkRoe result) {
            if (result) {
              nested_wait.Finish(Roe<void>());
            } else {
              nested_wait.Finish(Roe<void>(Error(result.error().message)));
            }
          });
      const auto nested_deadline = Clock::now() + std::chrono::milliseconds(10000);
      while (Clock::now() < nested_deadline && !nested_wait.IsSettled()) {
        if (io_pump_) {
          io_pump_();
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      auto nested = nested_wait.Wait(std::chrono::milliseconds(10000), Error("amp nested session timed out"));
      if (!nested) {
        continue;
      }
      (void)hops_.Install(target_peer_id, relay_key, target_protocol, bridged->session, id);
      return {};
    }

    if (register_endpoint && !bridged->resolved_multiaddr.empty()) {
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
