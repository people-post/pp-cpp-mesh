#include "feature/calls/AmpCircuitHopReach.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"

#include "amp/link/PeerLink.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/SettledWait.h"

#include <chrono>
#include <optional>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

AmpCircuitHopReach::AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops,
                                       IChatPeerLinks& links, IoPump io_pump,
                                       CollectRelays collect_relays, TryPunch try_punch,
                                       TryPunchViaIntroducer try_punch_via_introducer)
    : circuit_(circuit), hops_(hops), links_(links), io_pump_(std::move(io_pump)),
      collect_relays_(std::move(collect_relays)), try_punch_(std::move(try_punch)),
      try_punch_via_introducer_(std::move(try_punch_via_introducer)) {}

Roe<void> AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  if (hop_peer_id.empty()) {
    return Error("missing hop peer");
  }
  if (hops_.Find(hop_peer_id, kMediaRelayProtocolId)) {
    return {};
  }
  if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint) {
    return {};
  }
  if (try_punch_) {
    if (auto punched = try_punch_(hop_peer_id); punched) {
      if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint || links_.IsConnected(hop_peer_id)) {
        return {};
      }
    }
  }
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
  if (hops_.Find(peer_key, kCircuitCarrierProtocolId) && links_.IsConnected(peer_key)) {
    return {};
  }
  if (try_punch_) {
    if (auto punched = try_punch_(peer_key); punched) {
      if (links_.IsConnected(peer_key) || links_.GetLinkSnapshot(peer_key).has_endpoint) {
        return {};
      }
    }
  }
  return EnsureViaCircuit(peer_key, kCircuitCarrierProtocolId, /*register_endpoint=*/false,
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

Roe<void> AmpCircuitHopReach::TryUpgradeToDirect(const std::string& peer_key) {
  if (peer_key.empty()) {
    return Error("missing upgrade peer");
  }
  if (!try_punch_via_introducer_) {
    return Error("circuit upgrade punch unavailable");
  }

  std::optional<AmpCircuitHopRegistry::Hop> hop = hops_.Find(peer_key, kCircuitCarrierProtocolId);
  std::string protocol = kCircuitCarrierProtocolId;
  if (!hop) {
    hop = hops_.Find(peer_key, kMediaRelayProtocolId);
    protocol = kMediaRelayProtocolId;
  }
  if (!hop) {
    return Error("no circuit hop to upgrade");
  }

  const std::string relay_key = hop->relay_peer_key;
  const CircuitTunnelId tunnel_id = hop->tunnel_id;

  if (auto punched = try_punch_via_introducer_(relay_key, peer_key); !punched) {
    return punched;
  }

  // Require PeerId address-book upsert (direct dialable) before demoting the circuit.
  // IsConnected alone may still be the nested/carrier path.
  if (!links_.GetLinkSnapshot(peer_key).has_endpoint) {
    return Error("upgrade punch did not yield a direct path");
  }

  return DemoteCircuitHop(peer_key, protocol, tunnel_id);
}

Roe<void> AmpCircuitHopReach::DemoteCircuitHop(const std::string& peer_key, const std::string& target_protocol,
                                               CircuitTunnelId tunnel_id) {
  if (tunnel_id) {
    circuit_.CancelTunnel(tunnel_id);
  }
  hops_.Clear(peer_key, target_protocol);
  // If other protocols remain for this peer, leave them; Clear(peer) only when none left.
  if (!hops_.HasAny(peer_key)) {
    hops_.Clear(peer_key);
  }
  return {};
}


} // namespace pbr
