#include "feature/calls/AmpCircuitHopReach.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "common/directory/MeshHopDial.h"
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
  auto run = [this, hop_peer_id, on_done = std::move(on_done)]() mutable {
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
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
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
  auto run = [this, peer_key, allow_circuit, on_done = std::move(on_done)]() mutable {
  if (links_.IsConnected(peer_key)) {
    on_done(Roe<void>());
    return;
  }
  // has_endpoint alone is not Connected — call-media OpenChannel hangs if we skip dial
  // (dogfood 612b: via_ok=1 with connected=0). Under dual-NAT, punch sync often registers a
  // *private* advertise MA; EnsureAssociation on that MA burns the dial budget and can drop the
  // hop assoc (hard-w5 Phase-2). Prefer circuit (peer-id-only nested) then punch over direct dial.
  //
  // Dogfood 130521: punch∥circuit on the same ADP UDP path (introducer often = circuit relay)
  // overlapped OpenChannel after EnsureAssociation sendto-miss and AVd ~10s with no tunnel log.
  // Circuit first; punch only if circuit misses. SoftMigrate can still upgrade later.
  //
  // Answerer reverse-dial (allow_circuit=false): punch only and wait. Circuit StartBridge to the
  // offerer fails with "endpoint not registered" on fleet seeds that do not see the offerer
  // (dogfood 072a7425); offerer dials the reserved answerer after inbound grace instead.
  const uint64_t gen = deferred_.Snapshot();
  auto aborted = [tok = deferred_.token(), gen]() {
    return !DeferredSelf::Alive(tok, gen);
  };
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish = [settled, on_done = std::move(on_done)](Roe<void> result) mutable {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(result));
    }
  };
  auto run_punch = [this, peer_key, finish, settled, aborted](std::shared_ptr<Error> last_err) mutable {
    if (aborted()) {
      finish(Error("circuit hop aborted"));
      return;
    }
    if (!try_punch_) {
      if (links_.IsConnected(peer_key)) {
        finish(Roe<void>());
      } else {
        finish(last_err ? *last_err : Error("call-media circuit reach failed"));
      }
      return;
    }
    AmpReachLog().info << "TryEnsureCallMediaReachable punch after circuit miss target=" << peer_key;
    try_punch_(peer_key, [this, peer_key, finish, settled, last_err, aborted](Roe<void>) mutable {
      if (settled->load(std::memory_order_acquire)) {
        return;
      }
      if (aborted()) {
        finish(Error("circuit hop aborted"));
        return;
      }
      if (links_.IsConnected(peer_key)) {
        finish(Roe<void>());
        return;
      }
      finish(last_err ? *last_err : Error("call peer not connected after punch"));
    });
  };

  if (!allow_circuit) {
    AmpReachLog().info << "TryEnsureCallMediaReachable punch-only (no circuit dial) target="
                       << peer_key;
    run_punch(nullptr);
    return;
  }

  EnsureViaCircuitAsync(
      peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
      /*nested_session=*/true,
      [this, peer_key, finish, settled, run_punch, aborted](Roe<void> via) mutable {
        if (settled->load(std::memory_order_acquire)) {
          return;
        }
        if (aborted()) {
          finish(Error("circuit hop aborted"));
          return;
        }
        if (links_.IsConnected(peer_key)) {
          finish(Roe<void>());
          return;
        }
        // Do not fall through to punch after an intentional abort (Leave / ConnectFailed).
        if (!via && via.error().message.find("aborted") != std::string::npos) {
          finish(via);
          return;
        }
        auto last_err = std::make_shared<Error>(
            !via ? via.error() : Error("call peer not connected after circuit"));
        run_punch(std::move(last_err));
      });
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::EnsureViaCircuitAsync(const std::string& target_peer_id,
                                               const std::string& target_protocol,
                                               const bool register_endpoint, const bool nested_session,
                                               std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  // PeerLinkManager is Amp-IO only. CallMedia Connect ticks on Coordinator while MeshPump
  // Ticks on IO — ClearDialBackoff / snapshot / OpenChannel off-strand AVs around dial
  // timeout (dogfood 085210, ~8s after StartBridge).
  auto run = [this, target_peer_id, target_protocol, register_endpoint, nested_session,
              on_done = std::move(on_done)]() mutable {
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
  auto collected = collect_relays_(target_peer_id);
  if (collected.empty()) {
    AmpReachLog().info << "EnsureViaCircuit no dialable circuit relays target=" << target_peer_id
                       << " nested=" << (nested_session ? 1 : 0);
    on_done(Error("no dialable circuit relays"));
    return;
  }

  std::string sticky;
  if (auto hop = hops_.Find(target_peer_id, target_protocol)) {
    sticky = hop->relay_peer_key;
  } else if (!last_good_relay_peer_key_.empty()) {
    sticky = last_good_relay_peer_key_;
  }
  auto relays = std::make_shared<std::vector<std::string>>(OrderCircuitRelayAttempts(
      std::move(collected), sticky,
      [this](const std::string& key) { return links_.IsConnected(key); }));

  AmpReachLog().info << "EnsureViaCircuit start target=" << target_peer_id
                     << " protocol=" << target_protocol << " nested=" << (nested_session ? 1 : 0)
                     << " relays=" << relays->size() << " sticky=" << sticky
                     << " envelope_ms=" << kCircuitReachEnvelopeMs
                     << " max_bridges=" << kCircuitMaxStartBridgeAttempts;

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
  auto bridges_started = std::make_shared<std::size_t>(0);
  auto sticky_retried = std::make_shared<bool>(false);
  const auto envelope_deadline = Clock::now() + std::chrono::milliseconds(kCircuitReachEnvelopeMs);
  const uint64_t gen = deferred_.Snapshot();
  auto aborted = [tok = deferred_.token(), gen]() {
    return !DeferredSelf::Alive(tok, gen);
  };
  auto try_relay = std::make_shared<std::function<void(size_t)>>();
  *try_relay = [this, target_peer_id, target_protocol, register_endpoint, nested_session, bridge_target,
                relays, last_fail, bridges_started, sticky_retried, sticky, envelope_deadline, aborted,
                try_relay, on_done = std::move(on_done)](size_t index) mutable {
    if (aborted()) {
      AmpReachLog().info << "EnsureViaCircuit aborted target=" << target_peer_id << " index=" << index;
      on_done(Error("circuit hop aborted"));
      return;
    }
    if (index >= relays->size()) {
      AmpReachLog().info << "EnsureViaCircuit exhausted relays target=" << target_peer_id
                         << " tried_bridges=" << *bridges_started << " last=" << *last_fail;
      on_done(Error(*last_fail));
      return;
    }
    if (*bridges_started >= kCircuitMaxStartBridgeAttempts) {
      AmpReachLog().info << "EnsureViaCircuit bridge cap target=" << target_peer_id
                         << " bridges=" << *bridges_started << " last=" << *last_fail;
      on_done(Error(*last_fail));
      return;
    }
    const int64_t remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     envelope_deadline - Clock::now())
                                     .count();
    const int64_t bridge_timeout_ms = CircuitStartBridgeTimeoutMs(remaining_ms, nested_session);
    if (bridge_timeout_ms <= 0) {
      AmpReachLog().info << "EnsureViaCircuit envelope exhausted target=" << target_peer_id
                         << " remaining_ms=" << remaining_ms << " bridges=" << *bridges_started
                         << " last=" << *last_fail;
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
    // Defense: capability ingest can leave Preferred as /ip4/0.0.0.0 (dogfood 084055). Connected
    // relays are peer-id-only safe; otherwise skip undialable Preferred before StartBridge.
    if (!links_.IsConnected(relay_key)) {
      if (auto relay_ma = links_.PreferredMultiaddr(relay_key);
          relay_ma && !CircuitHopMultiaddrIsUdpDialable(*relay_ma)) {
        AmpReachLog().info << "EnsureViaCircuit skip relay=" << relay_key
                           << " reason=undialable_preferred preferred_ma=" << *relay_ma;
        *last_fail = "circuit hop reach failed: relay preferred undialable";
        (*try_relay)(index + 1);
        return;
      }
    }

    // Dogfood: EnsureAssociation on the call peer can leave relays in DialInBackoff;
    // clear before StartBridge so circuit can dial the hop. Do not AbortInflightDial —
    // that ScheduleDropLink's a Backoff link while FinishDial's drop is still pending;
    // EnsureAssociation then DropLink-sync + redial, and Tick destroys the new dial (131904).
    links_.ClearDialBackoff(relay_key);

    AmpReachLog().info << "EnsureViaCircuit try relay=" << relay_key << " index=" << index
                       << " target=" << target_peer_id << " bridge_timeout_ms=" << bridge_timeout_ms
                       << " bridges_started=" << *bridges_started
                       << " remaining_ms=" << remaining_ms;
    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto tunnel_id = std::make_shared<CircuitTunnelId>();
    // After a miss, FinishDial only ScheduleDropLink's. Nested Drive under an active PostToIo
    // drain skips the queue (MeshRuntime::pumping_) but Tick still flushes drops. Then defer
    // the next StartBridge onto PostToIo so BeginOutbound is not stacked on the finish cb.
    auto advance_relay = std::make_shared<std::function<void(size_t, CircuitTunnelId)>>();
    *advance_relay =
        [this, try_relay, aborted, on_done](size_t next_index, CircuitTunnelId id) mutable {
          ClearInflightTunnel(id);
          if (id) {
            circuit_.CancelTunnel(id);
          }
          if (aborted()) {
            on_done(Error("circuit hop aborted"));
            return;
          }
          auto go = [try_relay, next_index]() { (*try_relay)(next_index); };
          if (post_io_) {
            // MeshPump owns Drive — defer next StartBridge; do not nested-Tick.
            post_io_(std::move(go));
          } else {
            go();
          }
        };
    auto on_bridge = std::make_shared<std::function<void(Roe<CircuitTunnelBridgeResult>)>>();
    *on_bridge = [this, target_peer_id, target_protocol, register_endpoint, nested_session, relay_key,
                  sticky, settled, tunnel_id, last_fail, bridges_started, sticky_retried,
                  envelope_deadline, aborted, try_relay, index, advance_relay,
                  on_done](Roe<CircuitTunnelBridgeResult> result) mutable {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      const CircuitTunnelId id = *tunnel_id;
      ClearInflightTunnel(id);
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
        const std::string relay_ma = links_.PreferredMultiaddr(relay_key).value_or("");
        AmpReachLog().info << "EnsureViaCircuit tunnel miss relay=" << relay_key
                           << " preferred_ma=" << relay_ma << " err=" << *last_fail;
        const bool fast_fail = CircuitBridgeErrorIsFastFail(*last_fail);
        const bool not_reg = last_fail->find("not registered") != std::string::npos;
        auto go_same = [advance_relay, index, id]() { (*advance_relay)(index, id); };
        // Same-relay not-reg: hop already event-waited; answerer may still be parking (H010).
        // Do not require sticky — first connect often has none.
        if (CircuitShouldRetrySameRelayOnNotReg(not_reg, *bridges_started)) {
          AmpReachLog().info << "EnsureViaCircuit not-reg retry same relay=" << relay_key
                             << " bridges=" << *bridges_started;
          go_same();
          return;
        }
        if (CircuitShouldRetryStickyOnce(relay_key, sticky, *sticky_retried, fast_fail,
                                         *bridges_started)) {
          *sticky_retried = true;
          AmpReachLog().info << "EnsureViaCircuit sticky retry once relay=" << relay_key;
          go_same();
          return;
        }
        (*advance_relay)(index + 1, id);
        return;
      }
      auto session = result->session;
      const std::string resolved = result->resolved_multiaddr;

      if (nested_session) {
        // Keep tunnel noted until nested settles so AbortPending can hard-cancel mid-Establish.
        NoteInflightTunnel(id);
        auto nested_settled = std::make_shared<std::atomic<bool>>(false);
        const int64_t nest_ms = CircuitNestedEstablishTimeoutMs(
            std::chrono::duration_cast<std::chrono::milliseconds>(envelope_deadline - Clock::now())
                .count());
        if (nest_ms <= 0) {
          *last_fail = "circuit hop reach failed: nested timeout";
          AmpReachLog().info << "EnsureViaCircuit nested envelope miss relay=" << relay_key;
          (*advance_relay)(index + 1, id);
          return;
        }
        const auto nested_deadline = Clock::now() + std::chrono::milliseconds(nest_ms);
        links_.EstablishNestedOverCarrier(
            target_peer_id, session, true,
            [this, target_peer_id, target_protocol, relay_key, id, session, nested_settled, last_fail,
             aborted, advance_relay, index, on_done](IChatPeerLinks::LinkRoe nested) mutable {
              if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                return;
              }
              ClearInflightTunnel(id);
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
                (*advance_relay)(index + 1, id);
                return;
              }
              (void)hops_.Install(target_peer_id, relay_key, target_protocol, session, id);
              last_good_relay_peer_key_ = relay_key;
              if (on_relay_chosen_) {
                on_relay_chosen_(relay_key);
              }
              AmpReachLog().info << "EnsureViaCircuit nested ok relay=" << relay_key
                                 << " target=" << target_peer_id;
              on_done(Roe<void>());
            });
        AmpScheduleUntilSettled(post_io_, io_pump_, nested_settled, nested_deadline,
                                [this, nested_settled, last_fail, advance_relay, index, id = id,
                                 relay_key, aborted, on_done]() {
                                  if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
                                    return;
                                  }
                                  ClearInflightTunnel(id);
                                  if (aborted()) {
                                    if (id) {
                                      circuit_.CancelTunnel(id);
                                    }
                                    on_done(Error("circuit hop aborted"));
                                    return;
                                  }
                                  *last_fail = "circuit hop reach failed: nested timeout";
                                  AmpReachLog().info << "EnsureViaCircuit nested timeout relay=" << relay_key
                                                     << " cancelling tunnel before next relay";
                                  (*advance_relay)(index + 1, id);
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
        if (id) {
          circuit_.CancelTunnel(id);
        }
        on_done(installed);
        return;
      }
      last_good_relay_peer_key_ = relay_key;
      if (on_relay_chosen_) {
        on_relay_chosen_(relay_key);
      }
      AmpReachLog().info << "EnsureViaCircuit ok relay=" << relay_key << " target=" << target_peer_id;
      on_done(Roe<void>());
    };

    ++(*bridges_started);
    *tunnel_id = circuit_.StartBridge(
        relay_key, bridge_target, {}, {},
        [on_bridge](Roe<CircuitTunnelBridgeResult> result) {
          if (on_bridge && *on_bridge) {
            (*on_bridge)(std::move(result));
          }
        },
        static_cast<int>(bridge_timeout_ms));
    if (!*tunnel_id) {
      *last_fail = "circuit hop reach failed: StartBridge rejected";
      AmpReachLog().info << "EnsureViaCircuit StartBridge reject relay=" << relay_key;
      (*advance_relay)(index + 1, {});
      return;
    }
    NoteInflightTunnel(*tunnel_id);

    // Outer settle slightly past StartBridge so TickDeadlines can finish TearDown first.
    const auto settle_deadline =
        Clock::now() + std::chrono::milliseconds(bridge_timeout_ms + 500);
    AmpScheduleUntilSettled(post_io_, io_pump_, settled, settle_deadline,
                            [this, settled, last_fail, advance_relay, index, tunnel_id, relay_key,
                             aborted, on_done]() {
                              if (settled->exchange(true, std::memory_order_acq_rel)) {
                                return;
                              }
                              // Dogfood 997c1c6f / 131904: waiter while OpenChannel live; cancel and
                              // flush pending drops before the next relay dial.
                              if (aborted()) {
                                ClearInflightTunnel(*tunnel_id);
                                if (*tunnel_id) {
                                  circuit_.CancelTunnel(*tunnel_id);
                                }
                                on_done(Error("circuit hop aborted"));
                                return;
                              }
                              *last_fail = "circuit hop reach failed: tunnel timeout";
                              AmpReachLog().info << "EnsureViaCircuit tunnel timeout relay=" << relay_key
                                                 << " cancelling tunnel before next relay";
                              // Timeouts are not sticky-retry — hop was dialing / hung.
                              (*advance_relay)(index + 1, *tunnel_id);
                            });
  };
  (*try_relay)(0);
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::NoteInflightTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  inflight_tunnel_value_.store(id.value, std::memory_order_release);
}

void AmpCircuitHopReach::ClearInflightTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  uint64_t expected = id.value;
  (void)inflight_tunnel_value_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

CircuitTunnelId AmpCircuitHopReach::TakeInflightTunnel() {
  const uint64_t value = inflight_tunnel_value_.exchange(0, std::memory_order_acq_rel);
  return CircuitTunnelId{value};
}

void AmpCircuitHopReach::AbortPending() {
  deferred_.Invalidate();
  const CircuitTunnelId id = TakeInflightTunnel();
  if (id) {
    circuit_.CancelTunnel(id);
  }
  AmpReachLog().info << "AbortPending gen=" << deferred_.Snapshot()
                     << " cancelled_tunnel=" << (id ? 1 : 0);
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
