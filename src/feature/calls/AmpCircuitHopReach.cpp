#include "feature/calls/AmpCircuitHopReach.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"

#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/SettledWait.h"
#include "common/Logger.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

logging::Logger& AmpReachLog() {
  static logging::Logger log = logging::getLogger("AmpCircuitHopReach");
  return log;
}

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
                                                          std::function<void(Roe<void>)> on_done,
                                                          const bool allow_circuit) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Error("missing call peer"));
    return;
  }
  if (links_.IsConnected(peer_key)) {
    on_done(Roe<void>());
    return;
  }
  // has_endpoint alone is not Connected — call-media OpenChannel hangs if we skip dial
  // (dogfood 612b: via_ok=1 with connected=0). Under dual-NAT, punch sync often registers a
  // *private* advertise MA; EnsureAssociation on that MA burns the dial budget and can drop the
  // hop assoc (hard-w5 Phase-2). Prefer punch∥circuit (peer-id-only nested) over direct dial.
  if (hops_.Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId) && links_.IsConnected(peer_key)) {
    on_done(Roe<void>());
    return;
  }
  // Dogfood 8b452388: sequential punch (window+4s) left only ~4s of the Bridge dial budget for
  // StartBridge+nested — AbortPending killed the in-flight hop. Run punch and circuit in parallel;
  // first Connected wins. SoftMigrate still prefers punched path when it wins the race.
  //
  // Answerer reverse-dial (allow_circuit=false): punch only and wait. Circuit StartBridge to the
  // offerer fails with "endpoint not registered" on fleet seeds that do not see the offerer
  // (dogfood 072a7425); offerer dials the reserved answerer after inbound grace instead.
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto punch_done = std::make_shared<std::atomic<bool>>(false);
  auto circuit_done = std::make_shared<std::atomic<bool>>(false);
  auto last_err = std::make_shared<Error>(Error("call-media circuit reach failed"));
  auto finish = [settled, on_done = std::move(on_done)](Roe<void> result) mutable {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(result));
    }
  };
  auto maybe_finish_miss =
      [this, peer_key, finish, punch_done, circuit_done, last_err, settled]() mutable {
        if (settled->load(std::memory_order_acquire)) {
          return;
        }
        if (!punch_done->load(std::memory_order_acquire) ||
            !circuit_done->load(std::memory_order_acquire)) {
          return;
        }
        if (links_.IsConnected(peer_key)) {
          finish(Roe<void>());
          return;
        }
        finish(*last_err);
      };

  if (allow_circuit) {
    EnsureViaCircuitAsync(
        peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
        /*nested_session=*/true,
        [this, peer_key, finish, circuit_done, last_err, settled,
         maybe_finish_miss](Roe<void> via) mutable {
          circuit_done->store(true, std::memory_order_release);
          if (settled->load(std::memory_order_acquire)) {
            return;
          }
          if (links_.IsConnected(peer_key)) {
            finish(Roe<void>());
            return;
          }
          if (!via) {
            *last_err = via.error();
          } else {
            *last_err = Error("call peer not connected after circuit");
          }
          maybe_finish_miss();
        });
  } else {
    circuit_done->store(true, std::memory_order_release);
    AmpReachLog().info << "TryEnsureCallMediaReachable punch-only (no circuit dial) target="
                       << peer_key;
  }

  if (!try_punch_) {
    punch_done->store(true, std::memory_order_release);
    maybe_finish_miss();
    return;
  }
  try_punch_(peer_key, [this, peer_key, finish, punch_done, settled, maybe_finish_miss](Roe<void>) mutable {
    punch_done->store(true, std::memory_order_release);
    if (settled->load(std::memory_order_acquire)) {
      return;
    }
    if (links_.IsConnected(peer_key)) {
      finish(Roe<void>());
      return;
    }
    maybe_finish_miss();
  });
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
    AmpReachLog().info << "EnsureViaCircuit no dialable circuit relays target=" << target_peer_id
                       << " nested=" << (nested_session ? 1 : 0);
    on_done(Error("no dialable circuit relays"));
    return;
  }
  AmpReachLog().info << "EnsureViaCircuit start target=" << target_peer_id
                     << " protocol=" << target_protocol << " nested=" << (nested_session ? 1 : 0)
                     << " relays=" << relays->size();

  CircuitBridgeTarget bridge_target;
  bridge_target.target_peer_id = target_peer_id;
  bridge_target.target_protocol = target_protocol;
  // Nested call-media: peer-id-only. Punch/sync often registers the peer's *private*
  // advertise MA on the dialer; sending that as target_multiaddr makes the hop
  // overwrite its SNAT-learned book entry and fail dual-NAT (dogfood / hard-w5 Phase-2).
  // Non-nested media-relay may still use PreferredMultiaddr when the dialer knows a path.
  if (!nested_session) {
    if (auto ma = links_.PreferredMultiaddr(target_peer_id)) {
      bridge_target.target_multiaddr = *ma;
    }
  }

  auto last_fail = std::make_shared<std::string>("circuit hop reach failed");
  const uint64_t gen = abort_gen_.load(std::memory_order_acquire);
  auto aborted = [this, gen]() {
    return abort_gen_.load(std::memory_order_acquire) != gen;
  };
  auto try_relay = std::make_shared<std::function<void(size_t)>>();
  *try_relay = [this, target_peer_id, target_protocol, register_endpoint, nested_session, bridge_target,
                relays, last_fail, aborted, try_relay, on_done = std::move(on_done)](size_t index) mutable {
    if (aborted()) {
      AmpReachLog().info << "EnsureViaCircuit aborted target=" << target_peer_id << " index=" << index;
      on_done(Error("circuit hop aborted"));
      return;
    }
    if (index >= relays->size()) {
      AmpReachLog().info << "EnsureViaCircuit exhausted relays target=" << target_peer_id
                         << " tried=" << relays->size() << " last=" << *last_fail;
      on_done(Error(*last_fail));
      return;
    }
    const std::string relay_key = (*relays)[index];
    if (relay_key == target_peer_id) {
      AmpReachLog().info << "EnsureViaCircuit skip self-relay target=" << target_peer_id;
      *last_fail = "circuit hop reach failed: relay is target";
      (*try_relay)(index + 1);
      return;
    }
    if (!links_.GetLinkSnapshot(relay_key).has_endpoint) {
      AmpReachLog().info << "EnsureViaCircuit skip relay=" << relay_key << " reason=!endpoint";
      *last_fail = "circuit hop reach failed: relay !endpoint";
      (*try_relay)(index + 1);
      return;
    }

    // Dogfood: EnsureAssociation on the call peer can leave unrelated relays in DialInBackoff;
    // clear before StartBridge so circuit can dial the hop (hard-lab MarkHot/Clear heal).
    links_.AbortInflightDial(relay_key);
    links_.ClearDialBackoff(relay_key);

    AmpReachLog().info << "EnsureViaCircuit try relay=" << relay_key << " index=" << index
                       << " target=" << target_peer_id;
    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto tunnel_id = std::make_shared<CircuitTunnelId>();
    auto on_bridge = std::make_shared<std::function<void(Roe<CircuitTunnelBridgeResult>)>>();
    *on_bridge = [this, target_peer_id, target_protocol, register_endpoint, nested_session, relay_key,
                  settled, tunnel_id, last_fail, aborted, try_relay, index,
                  on_done](Roe<CircuitTunnelBridgeResult> result) mutable {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      const CircuitTunnelId id = *tunnel_id;
      if (aborted()) {
        if (id) {
          circuit_.CancelTunnel(id);
        }
        AmpReachLog().info << "EnsureViaCircuit aborted after tunnel relay=" << relay_key;
        on_done(Error("circuit hop aborted"));
        return;
      }
      if (!result || !result->ok || !result->session) {
        *last_fail = !result ? ("circuit hop reach failed: tunnel " + result.error().message)
                             : (!result->ok ? ("circuit hop reach failed: tunnel rejected " + result->error)
                                            : "circuit hop reach failed: tunnel no session");
        AmpReachLog().info << "EnsureViaCircuit tunnel miss relay=" << relay_key
                           << " err=" << *last_fail;
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
            [this, target_peer_id, target_protocol, relay_key, id, session, nested_settled, last_fail,
             aborted, try_relay, index, on_done](IChatPeerLinks::LinkRoe nested) mutable {
              if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                return;
              }
              if (aborted()) {
                if (id) {
                  circuit_.CancelTunnel(id);
                }
                on_done(Error("circuit hop aborted"));
                return;
              }
              if (!nested) {
                *last_fail = "circuit hop reach failed: nested " + nested.error().message;
                AmpReachLog().info << "EnsureViaCircuit nested miss relay=" << relay_key
                                   << " target=" << target_peer_id << " err=" << *last_fail;
                (*try_relay)(index + 1);
                return;
              }
              (void)hops_.Install(target_peer_id, relay_key, target_protocol, session, id);
              AmpReachLog().info << "EnsureViaCircuit nested ok relay=" << relay_key
                                 << " target=" << target_peer_id;
              on_done(Roe<void>());
            });
        AmpScheduleUntilSettled(post_io_, io_pump_, nested_settled, nested_deadline,
                                [this, nested_settled, last_fail, aborted, try_relay, index, id = id,
                                 on_done]() {
                                  if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                                    return;
                                  }
                                  if (aborted()) {
                                    if (id) {
                                      circuit_.CancelTunnel(id);
                                    }
                                    on_done(Error("circuit hop aborted"));
                                    return;
                                  }
                                  *last_fail = "circuit hop reach failed: nested timeout";
                                  (*try_relay)(index + 1);
                                });
        return;
      }

      if (register_endpoint && !resolved.empty()) {
        (void)links_.RegisterEndpoint(target_peer_id, resolved);
      }
      auto installed = hops_.Install(target_peer_id, relay_key, target_protocol, session, id);
      if (!installed) {
        *last_fail = "circuit hop reach failed: install " + installed.error().message;
        AmpReachLog().info << "EnsureViaCircuit install miss relay=" << relay_key
                           << " err=" << *last_fail;
        on_done(installed);
        return;
      }
      AmpReachLog().info << "EnsureViaCircuit ok relay=" << relay_key << " target=" << target_peer_id;
      on_done(Roe<void>());
    };

    *tunnel_id = circuit_.StartBridge(
        relay_key, bridge_target, {}, {},
        [on_bridge](Roe<CircuitTunnelBridgeResult> result) {
          if (on_bridge && *on_bridge) {
            (*on_bridge)(std::move(result));
          }
        },
        8000);
    if (!*tunnel_id) {
      *last_fail = "circuit hop reach failed: StartBridge rejected";
      AmpReachLog().info << "EnsureViaCircuit StartBridge reject relay=" << relay_key;
      (*try_relay)(index + 1);
      return;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(10000);
    AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline,
                            [this, settled, last_fail, aborted, try_relay, index, tunnel_id, on_done]() {
                              if (settled->exchange(true, std::memory_order_acq_rel)) {
                                return;
                              }
                              if (aborted()) {
                                if (*tunnel_id) {
                                  circuit_.CancelTunnel(*tunnel_id);
                                }
                                on_done(Error("circuit hop aborted"));
                                return;
                              }
                              *last_fail = "circuit hop reach failed: tunnel timeout";
                              (*try_relay)(index + 1);
                            });
  };
  (*try_relay)(0);
}

void AmpCircuitHopReach::AbortPending() {
  const uint64_t next = abort_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
  AmpReachLog().info << "AbortPending gen=" << next;
}

Roe<void> AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  if (AppRuntime::IsShuttingDown()) {
    AmpReachLog().debug << "TryEnsureHopReachable rejected: shutting down";
    return Error("shutdown in progress");
  }
  SettledWait<void> wait;
  TryEnsureHopReachableAsync(hop_peer_id, [wait](Roe<void> value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Error("circuit hop reach timed out"));
}

Roe<void> AmpCircuitHopReach::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (AppRuntime::IsShuttingDown()) {
    AmpReachLog().debug << "TryEnsureCallMediaReachable rejected: shutting down";
    return Error("shutdown in progress");
  }
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
