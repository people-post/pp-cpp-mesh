#include "feature/calls/AmpCircuitHopReach.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"

#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "common/SettledWait.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

AmpCircuitHopReach::AmpCircuitHopReach(CircuitTunnelCoordinator& circuit, AmpCircuitHopRegistry& hops,
                                       IChatPeerLinks& links, IoPump io_pump,
                                       CollectRelays collect_relays, TryPunchAsync try_punch,
                                       TryPunchViaIntroducerAsync try_punch_via_introducer, IoPost post_io)
    : circuit_(circuit), hops_(hops), links_(links), io_pump_(std::move(io_pump)),
      post_io_(std::move(post_io)), collect_relays_(std::move(collect_relays)),
      try_punch_(std::move(try_punch)), try_punch_via_introducer_(std::move(try_punch_via_introducer)) {}

void AmpCircuitHopReach::TryEnsureHopReachableAsync(const std::string& hop_peer_id,
                                                    std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (hop_peer_id.empty()) {
    on_done(Error("missing hop peer"));
    return;
  }
  if (hops_.Find(hop_peer_id, kMediaRelayProtocolId)) {
    on_done(Roe<void>());
    return;
  }
  if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint) {
    on_done(Roe<void>());
    return;
  }
  auto after_punch = [this, hop_peer_id, on_done = std::move(on_done)](Roe<void> /*punched*/) mutable {
    if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint || links_.IsConnected(hop_peer_id)) {
      on_done(Roe<void>());
      return;
    }
    EnsureViaCircuitAsync(hop_peer_id, kMediaRelayProtocolId, /*register_endpoint=*/true,
                          /*nested_session=*/false, std::move(on_done));
  };
  if (try_punch_) {
    try_punch_(hop_peer_id, std::move(after_punch));
    return;
  }
  after_punch(Error("no punch"));
}

void AmpCircuitHopReach::TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                                          std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Error("missing call peer"));
    return;
  }
  if (links_.IsConnected(peer_key) || links_.GetLinkSnapshot(peer_key).has_endpoint) {
    on_done(Roe<void>());
    return;
  }
  if (hops_.Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId) && links_.IsConnected(peer_key)) {
    on_done(Roe<void>());
    return;
  }
  auto after_punch = [this, peer_key, on_done = std::move(on_done)](Roe<void> /*punched*/) mutable {
    if (links_.IsConnected(peer_key) || links_.GetLinkSnapshot(peer_key).has_endpoint) {
      on_done(Roe<void>());
      return;
    }
    EnsureViaCircuitAsync(peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
                          /*nested_session=*/true, std::move(on_done));
  };
  if (try_punch_) {
    try_punch_(peer_key, std::move(after_punch));
    return;
  }
  after_punch(Error("no punch"));
}

void AmpCircuitHopReach::EnsureViaCircuitAsync(const std::string& target_peer_id,
                                               const std::string& target_protocol,
                                               const bool register_endpoint, const bool nested_session,
                                               std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!circuit_.IsStarted()) {
    on_done(Error("amp circuit-relay not available"));
    return;
  }
  if (target_peer_id.empty() || target_protocol.empty()) {
    on_done(Error("amp circuit hop incomplete"));
    return;
  }
  if (!nested_session && hops_.Find(target_peer_id, target_protocol)) {
    on_done(Roe<void>());
    return;
  }
  if (nested_session && links_.IsConnected(target_peer_id)) {
    on_done(Roe<void>());
    return;
  }
  if (!nested_session && links_.GetLinkSnapshot(target_peer_id).has_endpoint) {
    on_done(Roe<void>());
    return;
  }
  if (!collect_relays_) {
    on_done(Error("no dialable circuit relays"));
    return;
  }
  auto relays = std::make_shared<std::vector<std::string>>(collect_relays_(target_peer_id));
  if (relays->empty()) {
    on_done(Error("no dialable circuit relays"));
    return;
  }

  CircuitBridgeTarget bridge_target;
  bridge_target.target_peer_id = target_peer_id;
  bridge_target.target_protocol = target_protocol;
  if (auto ma = links_.PreferredMultiaddr(target_peer_id)) {
    bridge_target.target_multiaddr = *ma;
  }

  auto try_relay = std::make_shared<std::function<void(size_t)>>();
  *try_relay = [this, target_peer_id, target_protocol, register_endpoint, nested_session, bridge_target,
                relays, try_relay, on_done = std::move(on_done)](size_t index) mutable {
    if (index >= relays->size()) {
      on_done(Error("circuit hop reach failed"));
      return;
    }
    const std::string relay_key = (*relays)[index];
    if (relay_key == target_peer_id || !links_.GetLinkSnapshot(relay_key).has_endpoint) {
      (*try_relay)(index + 1);
      return;
    }

    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto tunnel_id = std::make_shared<CircuitTunnelId>();
    auto on_bridge = std::make_shared<std::function<void(Roe<CircuitTunnelBridgeResult>)>>();
    *on_bridge = [this, target_peer_id, target_protocol, register_endpoint, nested_session, relay_key,
                  settled, tunnel_id, try_relay, index, on_done](Roe<CircuitTunnelBridgeResult> result) mutable {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      const CircuitTunnelId id = *tunnel_id;
      if (!result || !result->ok || !result->session) {
        (*try_relay)(index + 1);
        return;
      }
      auto session = result->session;
      const std::string resolved = result->resolved_multiaddr;

      if (nested_session) {
        auto nested_settled = std::make_shared<std::atomic<bool>>(false);
        const auto nested_deadline = Clock::now() + std::chrono::milliseconds(10000);
        links_.EstablishNestedOverCarrier(
            target_peer_id, session, true,
            [this, target_peer_id, target_protocol, relay_key, id, session, nested_settled, try_relay, index,
             on_done](IChatPeerLinks::LinkRoe nested) mutable {
              if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                return;
              }
              if (!nested) {
                (*try_relay)(index + 1);
                return;
              }
              (void)hops_.Install(target_peer_id, relay_key, target_protocol, session, id);
              on_done(Roe<void>());
            });
        AmpScheduleUntilSettled(post_io_, io_pump_, nested_settled, nested_deadline,
                                [nested_settled, try_relay, index]() {
                                  if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                                    return;
                                  }
                                  (*try_relay)(index + 1);
                                });
        return;
      }

      if (register_endpoint && !resolved.empty()) {
        (void)links_.RegisterEndpoint(target_peer_id, resolved);
      }
      auto installed = hops_.Install(target_peer_id, relay_key, target_protocol, session, id);
      if (!installed) {
        on_done(installed);
        return;
      }
      on_done(Roe<void>());
    };

    *tunnel_id = circuit_.StartBridge(
        relay_key, bridge_target, {}, {},
        [on_bridge](Roe<CircuitTunnelBridgeResult> result) { (*on_bridge)(std::move(result)); }, 8000);
    if (!*tunnel_id) {
      (*try_relay)(index + 1);
      return;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(10000);
    AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [settled, try_relay, index]() {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      (*try_relay)(index + 1);
    });
  };
  (*try_relay)(0);
}

Roe<void> AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  SettledWait<void> wait;
  TryEnsureHopReachableAsync(hop_peer_id, [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Error("circuit hop reach timed out"));
}

Roe<void> AmpCircuitHopReach::TryEnsureCallMediaReachable(const std::string& peer_key) {
  SettledWait<void> wait;
  TryEnsureCallMediaReachableAsync(peer_key, [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Error("call-media circuit reach timed out"));
}

void AmpCircuitHopReach::TryUpgradeToDirectAsync(const std::string& peer_key,
                                                 std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Error("missing upgrade peer"));
    return;
  }
  if (!try_punch_via_introducer_) {
    on_done(Error("circuit upgrade punch unavailable"));
    return;
  }

  std::optional<AmpCircuitHopRegistry::Hop> hop = hops_.Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId);
  std::string protocol = pp::amp::kAmpCircuitCarrierProtocolId;
  if (!hop) {
    hop = hops_.Find(peer_key, kMediaRelayProtocolId);
    protocol = kMediaRelayProtocolId;
  }
  if (!hop) {
    on_done(Error("no circuit hop to upgrade"));
    return;
  }

  const std::string relay_key = hop->relay_peer_key;
  const CircuitTunnelId tunnel_id = hop->tunnel_id;
  try_punch_via_introducer_(
      relay_key, peer_key,
      [this, peer_key, protocol, tunnel_id, on_done = std::move(on_done)](Roe<void> punched) mutable {
        if (!punched) {
          on_done(std::move(punched));
          return;
        }
        if (!links_.GetLinkSnapshot(peer_key).has_endpoint) {
          on_done(Error("upgrade punch did not yield a direct path"));
          return;
        }
        on_done(DemoteCircuitHop(peer_key, protocol, tunnel_id));
      });
}

Roe<void> AmpCircuitHopReach::TryUpgradeToDirect(const std::string& peer_key) {
  SettledWait<void> wait;
  TryUpgradeToDirectAsync(peer_key, [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Error("circuit upgrade timed out"));
}

Roe<void> AmpCircuitHopReach::DemoteCircuitHop(const std::string& peer_key, const std::string& target_protocol,
                                               CircuitTunnelId tunnel_id) {
  if (tunnel_id) {
    circuit_.CancelTunnel(tunnel_id);
  }
  hops_.Clear(peer_key, target_protocol);
  if (!hops_.HasAny(peer_key)) {
    hops_.Clear(peer_key);
  }
  return {};
}

} // namespace pbr
