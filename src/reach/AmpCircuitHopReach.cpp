#include "mesh/reach/AmpCircuitHopReach.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "mesh/types/MeshHopDial.h"
#include "mesh/l4/shared/L4ProtocolIds.h"
#include "mesh/shared/AmpParkUntil.h"
#include "common/SettledWait.h"
#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
namespace pp::mesh {
namespace {

using Clock = std::chrono::steady_clock;

logging::Logger& AmpReachLog() {
  static logging::Logger log = logging::getLogger("AmpCircuitHopReach");
  return log;
}

using Err = CircuitReachErr;
using Failure = CircuitReachFailure;

Failure Aborted() {
  return Failure::Of(Err::Aborted, "circuit hop aborted");
}

} // namespace

/**
 * One EnsureViaCircuit: the relays it may bridge through, best first, at most kCircuitParallelBridges
 * at once. The first bridge to ack wins: the nested link is established over it alone (no new bridge
 * meanwhile; one more acking then is cancelled), and once it is up every other bridge is cancelled. On
 * the Amp IO strand only.
 */
struct AmpCircuitHopReach::WalkBridge {
  std::string relay;
  CircuitTunnelId id;
  /** Answered, timed out or cancelled: whatever comes later is ignored. */
  std::shared_ptr<std::atomic<bool>> settled = std::make_shared<std::atomic<bool>>(false);
};

struct AmpCircuitHopReach::BridgeWalk {
  std::string target_peer_id;
  std::string target_protocol;
  bool register_endpoint = false;
  bool nested_session = false;
  CircuitBridgeTarget bridge_target;
  std::vector<std::string> relays;
  std::vector<std::string> advertised;
  std::string sticky;
  int64_t envelope_deadline_ms = 0;
  std::function<bool()> aborted;
  CircuitReachDone on_done;

  /** The next relay in `relays` to bridge through; `again` (same-relay retries) go before it. */
  size_t next = 0;
  std::vector<std::string> again;
  size_t bridges_started = 0;
  bool sticky_retried = false;
  std::vector<std::string> not_reg_retried;
  /** The last relay's miss: what the reach reports when it runs out. */
  Failure last_fail = Failure::Of(Err::RelayUnusable, "circuit hop reach failed");
  /** Bridges started and not answered yet. */
  std::vector<BridgePtr> out;
  /** A bridge acked and the nested link is being established over it. */
  bool nesting = false;
  bool done = false;
};

CircuitReachFailure AmpCircuitHopReach::WrapTunnelFailure(const CircuitClientCoordinator::Failure& tunnel) {
  Err code = Err::Generic;
  switch (tunnel.GetCode()) {
    case CircuitTunnelErr::NotStarted:
      code = Err::Unavailable;
      break;
    case CircuitTunnelErr::BadRequest:
      code = Err::BadRequest;
      break;
    case CircuitTunnelErr::RelayNoEndpoint:
      code = Err::RelayUnusable;
      break;
    case CircuitTunnelErr::RelayLinkFailed:
    case CircuitTunnelErr::ChannelFailed:
    case CircuitTunnelErr::Closed:
    case CircuitTunnelErr::BadReply:
      code = Err::RelayUnreachable;
      break;
    case CircuitTunnelErr::Timeout:
      code = Err::Timeout;
      break;
    case CircuitTunnelErr::Aborted:
      code = Err::Aborted;
      break;
    case CircuitTunnelErr::TargetNotReserved:
      code = Err::TargetNotReserved;
      break;
    case CircuitTunnelErr::TargetUnreachable:
      code = Err::TargetUnreachable;
      break;
    case CircuitTunnelErr::RelayBusy:
    case CircuitTunnelErr::RelayRefused:
      code = Err::RelayRefused;
      break;
    case CircuitTunnelErr::Ok:
    case CircuitTunnelErr::Generic:
      code = Err::Generic;
      break;
  }
  return Failure::Of(code, detail::AppendFrom("circuit hop reach failed", "tunnel", tunnel.message));
}

CircuitReachFailure AmpCircuitHopReach::WrapPunchFailure(const PunchIntroducerWalk::Failure& punch) {
  const Err code = punch.GetCode() == PunchIntroducerWalk::Err::Unavailable ? Err::Unavailable : Err::PunchFailed;
  return Failure::Of(code, detail::AppendFrom("circuit hop reach: punch failed", "punch", punch.message));
}

AmpCircuitHopReach::AmpCircuitHopReach(CircuitClientCoordinator& circuit, AmpCircuitHopRegistry& hops,
                                       IChatPeerLinks& links, IoPump io_pump, CollectRelays collect_relays,
                                       TryPunchAsync try_punch, TryPunchViaIntroducerAsync try_punch_via_introducer,
                                       IoPost post_io, IoAfter post_after)
    : circuit_(circuit),
      hops_(hops),
      links_(links),
      io_pump_(std::move(io_pump)),
      post_io_(std::move(post_io)),
      post_after_(std::move(post_after)),
      collect_relays_(std::move(collect_relays)),
      try_punch_(std::move(try_punch)),
      try_punch_via_introducer_(std::move(try_punch_via_introducer)) {}

void AmpCircuitHopReach::TryEnsureHopReachableAsync(const std::string& hop_peer_id, CircuitReachDone on_done) {
  if (!on_done) {
    return;
  }
  if (hop_peer_id.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing hop peer"));
    return;
  }
  auto run = [this, hop_peer_id, on_done = std::move(on_done)]() mutable {
    if (hops_.Find(hop_peer_id, kMediaRelayProtocolId)) {
      on_done(CircuitReachRoe());
      return;
    }
    if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint) {
      on_done(CircuitReachRoe());
      return;
    }
    auto after_punch = [this, hop_peer_id,
                        on_done = std::move(on_done)](PunchIntroducerWalk::WalkRoe /*punched*/) mutable {
      if (links_.GetLinkSnapshot(hop_peer_id).has_endpoint || links_.IsConnected(hop_peer_id)) {
        on_done(CircuitReachRoe());
        return;
      }
      EnsureViaCircuitAsync(hop_peer_id, kMediaRelayProtocolId, /*register_endpoint=*/true,
                            /*nested_session=*/false, std::move(on_done));
    };
    if (try_punch_) {
      try_punch_(hop_peer_id, std::move(after_punch));
      return;
    }
    after_punch(PunchIntroducerWalk::Failure::Of(PunchIntroducerWalk::Err::Unavailable, "no punch"));
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::TryEnsurePeerReachableAsync(const std::string& peer_key, CircuitReachDone on_done,
                                                     const bool allow_circuit) {
  TryEnsurePeerReachableViaAsync(peer_key, std::move(on_done), allow_circuit, {});
}

void AmpCircuitHopReach::TryEnsurePeerReachableViaAsync(const std::string& peer_key, CircuitReachDone on_done,
                                                        const bool allow_circuit,
                                                        std::vector<std::string> advertised_relays) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing peer"));
    return;
  }
  auto run = [this, peer_key, allow_circuit, advertised_relays = std::move(advertised_relays),
              on_done = std::move(on_done)]() mutable {
    if (links_.IsConnected(peer_key)) {
      on_done(CircuitReachRoe());
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
    auto aborted = [tok = deferred_.token(), gen]() { return !pp::Lifetime::Alive(tok, gen); };
    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto finish = [settled, on_done = std::move(on_done)](CircuitReachRoe result) mutable {
      if (settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      if (on_done) {
        on_done(std::move(result));
      }
    };
    auto run_punch = [this, peer_key, finish, settled, aborted](std::shared_ptr<Failure> last_err) mutable {
      if (aborted()) {
        finish(Aborted());
        return;
      }
      if (!try_punch_) {
        if (links_.IsConnected(peer_key)) {
          finish(CircuitReachRoe());
        } else {
          finish(last_err ? *last_err : Failure::Of(Err::NotConnected, "peer circuit reach failed"));
        }
        return;
      }
      AmpReachLog().info << "TryEnsurePeerReachable punch after circuit miss target=" << peer_key;
      try_punch_(peer_key,
                 [this, peer_key, finish, settled, last_err, aborted](PunchIntroducerWalk::WalkRoe punched) mutable {
                   if (settled->load(std::memory_order_acquire)) {
                     return;
                   }
                   if (aborted()) {
                     finish(Aborted());
                     return;
                   }
                   if (links_.IsConnected(peer_key)) {
                     finish(CircuitReachRoe());
                     return;
                   }
                   // The circuit's miss says more than the punch's (the punch was the last resort); without
                   // one, the punch's.
                   if (last_err) {
                     finish(*last_err);
                   } else if (!punched) {
                     finish(WrapPunchFailure(punched.error()));
                   } else {
                     finish(Failure::Of(Err::NotConnected, "peer not connected after punch"));
                   }
                 });
    };

    if (!allow_circuit) {
      AmpReachLog().info << "TryEnsurePeerReachable punch-only (no circuit dial) target=" << peer_key;
      run_punch(nullptr);
      return;
    }

    EnsureViaCircuitAsync(
        peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
        /*nested_session=*/true,
        [this, peer_key, finish, settled, run_punch, aborted](CircuitReachRoe via) mutable {
          if (settled->load(std::memory_order_acquire)) {
            return;
          }
          if (aborted()) {
            finish(Aborted());
            return;
          }
          if (links_.IsConnected(peer_key)) {
            finish(CircuitReachRoe());
            return;
          }
          // Do not fall through to punch after an intentional abort (Leave / ConnectFailed).
          if (!via && via.error().GetCode() == Err::Aborted) {
            finish(via);
            return;
          }
          auto last_err = std::make_shared<Failure>(
              !via ? via.error() : Failure::Of(Err::NotConnected, "peer not connected after circuit"));
          run_punch(std::move(last_err));
        },
        CircuitStandbyPriority::None, std::move(advertised_relays));
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::TryEnsureCircuitOnlyAsync(const std::string& peer_key, CircuitReachDone on_done,
                                                   std::vector<std::string> advertised_relays) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing peer"));
    return;
  }
  auto run = [this, peer_key, advertised_relays = std::move(advertised_relays),
              on_done = std::move(on_done)]() mutable {
    if (links_.IsConnected(peer_key)) {
      on_done(CircuitReachRoe());
      return;
    }
    AmpReachLog().info << "TryEnsureCircuitOnly circuit beside the direct dial target=" << peer_key;
    EnsureViaCircuitAsync(
        peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
        /*nested_session=*/true,
        [this, peer_key, on_done = std::move(on_done)](CircuitReachRoe via) mutable {
          if (links_.IsConnected(peer_key)) {
            on_done(CircuitReachRoe());
            return;
          }
          on_done(via ? CircuitReachRoe(Failure::Of(Err::NotConnected, "peer not connected after circuit")) : via);
        },
        CircuitStandbyPriority::None, std::move(advertised_relays));
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::TryEnsureRelayedAsync(const std::string& peer_key, CircuitReachDone on_done,
                                               const CircuitStandbyPriority standby_priority) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing peer"));
    return;
  }
  auto run = [this, peer_key, standby_priority, on_done = std::move(on_done)]() mutable {
    if (links_.IsConnectedRelayed(peer_key)) {
      on_done(CircuitReachRoe());
      return;
    }
    AmpReachLog().info << "TryEnsureRelayed circuit (a relayed link beside any direct one) target=" << peer_key;
    EnsureViaCircuitAsync(
        peer_key, pp::amp::kAmpCircuitCarrierProtocolId, /*register_endpoint=*/false,
        /*nested_session=*/true,
        [this, peer_key, on_done = std::move(on_done)](CircuitReachRoe via) mutable {
          if (links_.IsConnectedRelayed(peer_key)) {
            on_done(CircuitReachRoe());
            return;
          }
          on_done(via ? CircuitReachRoe(Failure::Of(Err::NotConnected, "no relayed link after circuit")) : via);
        },
        standby_priority);
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

void AmpCircuitHopReach::EnsureViaCircuitAsync(const std::string& target_peer_id, const std::string& target_protocol,
                                               const bool register_endpoint, const bool nested_session,
                                               CircuitReachDone on_done, const CircuitStandbyPriority standby_priority,
                                               std::vector<std::string> advertised_relays) {
  if (!on_done) {
    return;
  }
  // PeerLinkManager is Amp-IO only. CallMedia Connect ticks on Coordinator while MeshPump
  // Ticks on IO — ClearDialBackoff / snapshot / OpenChannel off-strand AVs around dial
  // timeout (dogfood 085210, ~8s after StartBridge).
  auto run = [this, target_peer_id, target_protocol, register_endpoint, nested_session, standby_priority,
              advertised_relays = std::move(advertised_relays), on_done = std::move(on_done)]() mutable {
    if (!circuit_.IsStarted()) {
      on_done(Failure::Of(Err::Unavailable, "amp circuit-relay not available"));
      return;
    }
    if (target_peer_id.empty() || target_protocol.empty()) {
      on_done(Failure::Of(Err::BadRequest, "amp circuit hop incomplete"));
      return;
    }
    if (!nested_session && hops_.Find(target_peer_id, target_protocol)) {
      on_done(CircuitReachRoe());
      return;
    }
    // A nested session exists to give the peer a relay carrier link: only one of those settles it
    // (a direct link beside it does not — relay standby / TX-only escalation want the relay).
    if (nested_session && links_.IsConnectedRelayed(target_peer_id)) {
      on_done(CircuitReachRoe());
      return;
    }
    if (!nested_session && links_.GetLinkSnapshot(target_peer_id).has_endpoint) {
      on_done(CircuitReachRoe());
      return;
    }
    if (!collect_relays_) {
      on_done(Failure::Of(Err::NoRelays, "no dialable circuit relays"));
      return;
    }
    auto collected = collect_relays_(target_peer_id);
    if (collected.empty()) {
      AmpReachLog().info << "EnsureViaCircuit no dialable circuit relays target=" << target_peer_id
                         << " nested=" << (nested_session ? 1 : 0);
      on_done(Failure::Of(Err::NoRelays, "no dialable circuit relays"));
      return;
    }

    std::string sticky;
    if (auto hop = hops_.Find(target_peer_id, target_protocol)) {
      sticky = hop->relay_peer_key;
    } else if (!last_good_relay_peer_key_.empty()) {
      sticky = last_good_relay_peer_key_;
    }
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    auto walk = std::make_shared<BridgeWalk>();
    // H013: the relays the target says it is parked on go first, whatever our own ranking or a recent
    // refusal says; only they earn a same-relay retry on `not registered`.
    walk->advertised = std::move(advertised_relays);
    std::vector<std::string> ordered =
        refusals_.Demote(OrderCircuitRelayAttempts(std::move(collected), sticky,
                                                   [this](const std::string& key) { return links_.IsConnected(key); }),
                         now_ms);
    if (peer_loads_) {
      // Open before busy, full / draining last (a full one refuses at once:
      // cheap, but not first).
      peer_loads_->OrderBest(ordered, MeshService::Relay, PeerLoadBook::NowMs());
    }
    walk->relays = PutAdvertisedRelaysFirst(std::move(ordered), walk->advertised);
    walk->target_peer_id = target_peer_id;
    walk->target_protocol = target_protocol;
    walk->register_endpoint = register_endpoint;
    walk->nested_session = nested_session;
    walk->sticky = sticky;
    walk->bridge_target.target_peer_id = target_peer_id;
    walk->bridge_target.target_protocol = target_protocol;
    walk->bridge_target.standby_priority = standby_priority;
    // Nested call-media: peer-id-only. Punch/sync often registers the peer's *private*
    // advertise MA on the dialer; sending that as target_multiaddr makes the hop
    // overwrite its SNAT-learned book entry and fail dual-NAT (dogfood / hard-w5 Phase-2).
    // Non-nested media-relay may still use PreferredMultiaddr when the dialer knows a path.
    if (!nested_session) {
      if (auto ma = links_.PreferredMultiaddr(target_peer_id)) {
        walk->bridge_target.target_multiaddr = *ma;
      }
    }
    walk->envelope_deadline_ms = NowMs() + kCircuitReachEnvelopeMs;
    walk->aborted = [tok = deferred_.token(), gen = deferred_.Snapshot()]() { return !pp::Lifetime::Alive(tok, gen); };
    walk->on_done = std::move(on_done);

    AmpReachLog().info << "EnsureViaCircuit start target=" << target_peer_id << " protocol=" << target_protocol
                       << " nested=" << (nested_session ? 1 : 0) << " relays=" << walk->relays.size()
                       << " advertised=" << walk->advertised.size() << " sticky=" << sticky
                       << " envelope_ms=" << kCircuitReachEnvelopeMs
                       << " max_bridges=" << kCircuitMaxStartBridgeAttempts << " at_once=" << kCircuitParallelBridges;
    FillBridges(walk);
  };
  if (post_io_) {
    post_io_(std::move(run));
  } else {
    run();
  }
}

int64_t AmpCircuitHopReach::NowMs() const {
  if (now_ms_) {
    return now_ms_();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void AmpCircuitHopReach::AfterMs(const int64_t delay_ms, const std::shared_ptr<std::atomic<bool>>& settled,
                                 std::function<void()> on_timeout) {
  AmpScheduleUntilSettled(post_io_, io_pump_, settled, Clock::now() + std::chrono::milliseconds(delay_ms),
                          std::move(on_timeout), post_after_);
}

bool AmpCircuitHopReach::RelayUsable(BridgeWalk& walk, const std::string& relay_key) {
  if (relay_key == walk.target_peer_id) {
    AmpReachLog().info << "EnsureViaCircuit skip self-relay target=" << walk.target_peer_id;
    walk.last_fail = Failure::Of(Err::RelayUnusable, "circuit hop reach failed: relay is target");
    return false;
  }
  if (!links_.GetLinkSnapshot(relay_key).has_endpoint) {
    AmpReachLog().info << "EnsureViaCircuit skip relay=" << relay_key << " reason=!endpoint";
    walk.last_fail = Failure::Of(Err::RelayUnusable, "circuit hop reach failed: relay !endpoint");
    return false;
  }
  // Defense: capability ingest can leave Preferred as /ip4/0.0.0.0 (dogfood 084055). Connected
  // relays are peer-id-only safe; otherwise skip undialable Preferred before StartBridge.
  if (!links_.IsConnected(relay_key)) {
    if (auto relay_ma = links_.PreferredMultiaddr(relay_key);
        relay_ma && !CircuitHopMultiaddrIsUdpDialable(*relay_ma)) {
      AmpReachLog().info << "EnsureViaCircuit skip relay=" << relay_key
                         << " reason=undialable_preferred preferred_ma=" << *relay_ma;
      walk.last_fail = Failure::Of(Err::RelayUnusable, "circuit hop reach failed: relay preferred undialable");
      return false;
    }
  }
  return true;
}

void AmpCircuitHopReach::FillBridges(const WalkPtr& walk) {
  if (walk->done) {
    return;
  }
  if (walk->aborted()) {
    AmpReachLog().info << "EnsureViaCircuit aborted target=" << walk->target_peer_id;
    FinishWalk(walk, Aborted());
    return;
  }
  using Stop = WalkStop;
  Stop stop = Stop::None;
  int64_t remaining_ms = 0;
  while (!walk->nesting && walk->out.size() < kCircuitParallelBridges) {
    if (walk->again.empty() && walk->next >= walk->relays.size()) {
      stop = Stop::Exhausted;
      break;
    }
    if (walk->bridges_started >= kCircuitMaxStartBridgeAttempts) {
      stop = Stop::Cap;
      break;
    }
    remaining_ms = walk->envelope_deadline_ms - NowMs();
    const int64_t bridge_timeout_ms = CircuitStartBridgeTimeoutMs(remaining_ms, walk->nested_session);
    if (bridge_timeout_ms <= 0) {
      stop = Stop::Envelope;
      break;
    }
    std::string relay_key;
    if (!walk->again.empty()) {
      relay_key = walk->again.front();
      walk->again.erase(walk->again.begin());
    } else {
      relay_key = walk->relays[walk->next++];
      if (!RelayUsable(*walk, relay_key)) {
        continue;
      }
    }
    LaunchBridge(walk, relay_key, bridge_timeout_ms, remaining_ms);
  }
  if (walk->out.empty() && !walk->nesting && !walk->done) {
    FinishWalkOutOfRelays(walk, stop, remaining_ms);
  }
}

void AmpCircuitHopReach::FinishWalkOutOfRelays(const WalkPtr& walk, const WalkStop stop, const int64_t remaining_ms) {
  using Stop = WalkStop;
  if (stop == Stop::Envelope) {
    AmpReachLog().info << "EnsureViaCircuit envelope exhausted target=" << walk->target_peer_id
                       << " remaining_ms=" << remaining_ms << " bridges=" << walk->bridges_started
                       << " last=" << walk->last_fail.message;
    // The envelope ran out: a timeout, unless a relay said something more telling.
    FinishWalk(walk, walk->last_fail.GetCode() == Err::RelayUnusable
                         ? Failure::Of(Err::Timeout, detail::AppendFrom("circuit hop reach: envelope exhausted", "last",
                                                                        walk->last_fail.message))
                         : walk->last_fail);
    return;
  }
  if (stop == Stop::Cap) {
    AmpReachLog().info << "EnsureViaCircuit bridge cap target=" << walk->target_peer_id
                       << " bridges=" << walk->bridges_started << " last=" << walk->last_fail.message;
  } else {
    AmpReachLog().info << "EnsureViaCircuit exhausted relays target=" << walk->target_peer_id
                       << " tried_bridges=" << walk->bridges_started << " last=" << walk->last_fail.message;
  }
  FinishWalk(walk, walk->last_fail);
}

void AmpCircuitHopReach::LaunchBridge(const WalkPtr& walk, const std::string& relay_key,
                                      const int64_t bridge_timeout_ms, const int64_t remaining_ms) {
  // Dogfood: EnsureAssociation on the call peer can leave relays in DialInBackoff;
  // clear before StartBridge so circuit can dial the hop. Do not AbortInflightDial —
  // that ScheduleDropLink's a Backoff link while FinishDial's drop is still pending;
  // EnsureAssociation then DropLink-sync + redial, and Tick destroys the new dial (131904).
  links_.ClearDialBackoff(relay_key);
  AmpReachLog().info << "EnsureViaCircuit try relay=" << relay_key << " target=" << walk->target_peer_id
                     << " bridge_timeout_ms=" << bridge_timeout_ms << " bridges_started=" << walk->bridges_started
                     << " in_flight=" << walk->out.size() << " remaining_ms=" << remaining_ms;
  auto bridge = std::make_shared<WalkBridge>();
  bridge->relay = relay_key;
  ++walk->bridges_started;
  bridge->id = circuit_.StartBridge(
      relay_key, walk->bridge_target, {}, {},
      [this, walk, bridge](CircuitClientCoordinator::BridgeRoe result) {
        OnBridgeFinished(walk, bridge, std::move(result));
      },
      static_cast<int>(bridge_timeout_ms));
  if (!bridge->id) {
    walk->last_fail = Failure::Of(Err::RelayUnusable, "circuit hop reach failed: StartBridge rejected");
    AmpReachLog().info << "EnsureViaCircuit StartBridge reject relay=" << relay_key;
    return;
  }
  walk->out.push_back(bridge);
  NoteInflightTunnel(bridge->id);
  // Outer settle slightly past StartBridge so TickDeadlines can finish TearDown first.
  AfterMs(bridge_timeout_ms + 500, bridge->settled, [this, walk, bridge]() { OnBridgeTimeout(walk, bridge); });
}

void AmpCircuitHopReach::ForgetBridge(BridgeWalk& walk, const WalkBridge& bridge) {
  std::erase_if(walk.out, [&bridge](const BridgePtr& b) { return b.get() == &bridge; });
  ClearInflightTunnel(bridge.id);
}

void AmpCircuitHopReach::RefillSoon(const WalkPtr& walk) {
  // After a miss, FinishDial only ScheduleDropLink's. Nested Drive under an active PostToIo
  // drain skips the queue (MeshRuntime::pumping_) but Tick still flushes drops. Then defer
  // the next StartBridge onto PostToIo so BeginOutbound is not stacked on the finish cb.
  if (post_io_) {
    post_io_([this, walk]() { FillBridges(walk); });
  } else {
    FillBridges(walk);
  }
}

void AmpCircuitHopReach::OnBridgeFinished(const WalkPtr& walk, const BridgePtr& bridge,
                                          CircuitClientCoordinator::BridgeRoe result) {
  const std::shared_ptr<pp::amp::ChannelSession> session = result ? result->session : nullptr;
  if (bridge->settled->exchange(true, std::memory_order_acq_rel)) {
    // Cancelled or timed out while its ack was on the way: CancelTunnel no longer reached it.
    ReleaseBridge(bridge->id, session);
    return;
  }
  ForgetBridge(*walk, *bridge);
  if (walk->done) {
    ReleaseBridge(bridge->id, session); // the walk ended meanwhile (another bridge won, or it ran out)
    return;
  }
  if (walk->aborted()) {
    ReleaseBridge(bridge->id, session);
    AmpReachLog().info << "EnsureViaCircuit aborted after tunnel relay=" << bridge->relay;
    FinishWalk(walk, Aborted());
    return;
  }
  if (!result || !result->session) {
    OnBridgeMissed(walk, *bridge, result);
    return;
  }
  if (walk->nesting) {
    // Another bridge acked first and the nested link is being established over it: one is enough.
    AmpReachLog().info << "EnsureViaCircuit second bridge acked while nesting relay=" << bridge->relay
                       << " (cancelled)";
    ReleaseBridge(bridge->id, session);
    return;
  }
  if (walk->nested_session) {
    NestOverBridge(walk, bridge, result->session);
    return;
  }
  InstallBridge(walk, *bridge, result->session, result->resolved_multiaddr);
}

void AmpCircuitHopReach::OnBridgeMissed(const WalkPtr& walk, const WalkBridge& bridge,
                                        const CircuitClientCoordinator::BridgeRoe& result) {
  const CircuitTunnelErr tunnel_code = !result ? result.error().GetCode() : CircuitTunnelErr::Generic;
  walk->last_fail = !result ? WrapTunnelFailure(result.error())
                            : Failure::Of(Err::RelayUnreachable, "circuit hop reach failed: tunnel no session");
  const std::string relay_ma = links_.PreferredMultiaddr(bridge.relay).value_or("");
  AmpReachLog().info << "EnsureViaCircuit tunnel miss relay=" << bridge.relay << " preferred_ma=" << relay_ma
                     << " err=" << walk->last_fail.message;
  if (!result && tunnel_code == CircuitTunnelErr::RelayRefused) {
    // Not ready, or not for us: the next reach tries it after the others (#379).
    refusals_.Note(bridge.relay,
                   std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
  }
  circuit_.CancelTunnel(bridge.id);
  const bool fast_fail = !result && CircuitBridgeMissIsFastFail(tunnel_code);
  const bool not_reg = tunnel_code == CircuitTunnelErr::TargetNotReserved;
  // Same-relay not-reg (H013): only on a relay the target advertised — its reservation there may
  // be renewing. Any other relay: advance, nothing would make the target appear there in time.
  if (CircuitShouldRetrySameRelayOnNotReg(not_reg, IsAdvertisedRelay(walk->advertised, bridge.relay),
                                          IsAdvertisedRelay(walk->not_reg_retried, bridge.relay))) {
    walk->not_reg_retried.push_back(bridge.relay);
    AmpReachLog().info << "EnsureViaCircuit not-reg retry advertised relay=" << bridge.relay
                       << " bridges=" << walk->bridges_started;
    // B27: announce this R1 so answerer PreferLateReserve re-parks while we retry once.
    if (on_relay_chosen_) {
      on_relay_chosen_(bridge.relay);
    }
    walk->again.push_back(bridge.relay);
  } else if (CircuitShouldRetryStickyOnce(bridge.relay, walk->sticky, walk->sticky_retried, fast_fail,
                                          walk->bridges_started)) {
    walk->sticky_retried = true;
    AmpReachLog().info << "EnsureViaCircuit sticky retry once relay=" << bridge.relay;
    walk->again.push_back(bridge.relay);
  }
  RefillSoon(walk);
}

void AmpCircuitHopReach::OnBridgeTimeout(const WalkPtr& walk, const BridgePtr& bridge) {
  if (bridge->settled->exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  ForgetBridge(*walk, *bridge);
  // Dogfood 997c1c6f / 131904: waiter while OpenChannel live; cancel and
  // flush pending drops before the next relay dial.
  circuit_.CancelTunnel(bridge->id);
  if (walk->done) {
    return;
  }
  if (walk->aborted()) {
    FinishWalk(walk, Aborted());
    return;
  }
  walk->last_fail = Failure::Of(Err::Timeout, "circuit hop reach failed: tunnel timeout");
  AmpReachLog().info << "EnsureViaCircuit tunnel timeout relay=" << bridge->relay
                     << " cancelling tunnel before next relay";
  // Timeouts are not sticky-retry — hop was dialing / hung.
  RefillSoon(walk);
}

void AmpCircuitHopReach::NestOverBridge(const WalkPtr& walk, const BridgePtr& bridge,
                                        const std::shared_ptr<pp::amp::ChannelSession>& session) {
  const int64_t nest_ms = CircuitNestedEstablishTimeoutMs(walk->envelope_deadline_ms - NowMs());
  if (nest_ms <= 0) {
    walk->last_fail = Failure::Of(Err::Timeout, "circuit hop reach failed: nested timeout");
    AmpReachLog().info << "EnsureViaCircuit nested envelope miss relay=" << bridge->relay;
    ReleaseBridge(bridge->id, session);
    RefillSoon(walk);
    return;
  }
  // Keep tunnel noted until nested settles so AbortPending can hard-cancel mid-Establish.
  NoteInflightTunnel(bridge->id);
  walk->nesting = true;
  auto nested_settled = std::make_shared<std::atomic<bool>>(false);
  links_.EstablishNestedOverCarrier(walk->target_peer_id, session, true,
                                    [this, walk, bridge, session, nested_settled](IChatPeerLinks::LinkRoe nested) {
                                      if (!nested_settled->exchange(true, std::memory_order_acq_rel)) {
                                        OnNestedDone(walk, *bridge, session, std::move(nested), false);
                                      }
                                    });
  AfterMs(nest_ms, nested_settled, [this, walk, bridge, session, nested_settled]() {
    if (nested_settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    AmpReachLog().info << "EnsureViaCircuit nested timeout relay=" << bridge->relay
                       << " cancelling tunnel before next relay";
    OnNestedDone(walk, *bridge, session, IChatPeerLinks::Failure::Of(IChatPeerLinks::Err::Generic, "nested timeout"),
                 true);
  });
}

void AmpCircuitHopReach::OnNestedDone(const WalkPtr& walk, const WalkBridge& bridge,
                                      const std::shared_ptr<pp::amp::ChannelSession>& session,
                                      IChatPeerLinks::LinkRoe nested, const bool timed_out) {
  ClearInflightTunnel(bridge.id);
  walk->nesting = false;
  if (walk->done || walk->aborted()) {
    ReleaseBridge(bridge.id, session);
    FinishWalk(walk, Aborted());
    return;
  }
  if (!nested) {
    walk->last_fail = timed_out ? Failure::Of(Err::NestedFailed, "circuit hop reach failed: nested timeout")
                                : Failure::Of(Err::NestedFailed, detail::AppendFrom("circuit hop reach failed: nested",
                                                                                    "link", nested.error().message));
    if (!timed_out) {
      AmpReachLog().info << "EnsureViaCircuit nested miss relay=" << bridge.relay << " target=" << walk->target_peer_id
                         << " err=" << walk->last_fail.message;
    }
    ReleaseBridge(bridge.id, session);
    RefillSoon(walk);
    return;
  }
  (void)hops_.Install(walk->target_peer_id, bridge.relay, walk->target_protocol, session, bridge.id);
  NoteRelayChosen(bridge.relay);
  AmpReachLog().info << "EnsureViaCircuit nested ok relay=" << bridge.relay << " target=" << walk->target_peer_id;
  FinishWalk(walk, CircuitReachRoe());
}

void AmpCircuitHopReach::InstallBridge(const WalkPtr& walk, const WalkBridge& bridge,
                                       const std::shared_ptr<pp::amp::ChannelSession>& session,
                                       const std::string& resolved) {
  if (walk->register_endpoint && !resolved.empty()) {
    (void)links_.RegisterEndpoint(walk->target_peer_id, resolved);
  }
  auto installed = hops_.Install(walk->target_peer_id, bridge.relay, walk->target_protocol, session, bridge.id);
  if (!installed) {
    walk->last_fail = Failure::Of(
        Err::Generic, detail::AppendFrom("circuit hop reach failed: install", "hops", installed.error().message));
    AmpReachLog().info << "EnsureViaCircuit install miss relay=" << bridge.relay << " err=" << walk->last_fail.message;
    ReleaseBridge(bridge.id, session);
    FinishWalk(walk, walk->last_fail);
    return;
  }
  NoteRelayChosen(bridge.relay);
  AmpReachLog().info << "EnsureViaCircuit ok relay=" << bridge.relay << " target=" << walk->target_peer_id;
  FinishWalk(walk, CircuitReachRoe());
}

void AmpCircuitHopReach::ReleaseBridge(const CircuitTunnelId id,
                                       const std::shared_ptr<pp::amp::ChannelSession>& session) {
  // A bridged tunnel is the client's no more (CancelTunnel only forgets it): its channel to the relay
  // is closed here, which ends the relay's splice — else the relay kept both legs open.
  if (session) {
    session->Close();
  }
  circuit_.CancelTunnel(id);
}

void AmpCircuitHopReach::NoteRelayChosen(const std::string& relay_key) {
  last_good_relay_peer_key_ = relay_key;
  refusals_.Clear(relay_key);
  if (on_relay_chosen_) {
    on_relay_chosen_(relay_key);
  }
}

void AmpCircuitHopReach::FinishWalk(const WalkPtr& walk, CircuitReachRoe result) {
  if (std::exchange(walk->done, true)) {
    return;
  }
  // The bridges still out lost: cancel them (the relay releases its legs; nothing was nested over them).
  for (const auto& bridge : std::exchange(walk->out, {})) {
    bridge->settled->store(true, std::memory_order_release);
    ClearInflightTunnel(bridge->id);
    circuit_.CancelTunnel(bridge->id);
    AmpReachLog().info << "EnsureViaCircuit cancel the other bridge relay=" << bridge->relay;
  }
  if (auto done = std::exchange(walk->on_done, nullptr)) { // exchange: a moved-from std::function may keep its target
    done(std::move(result));
  }
}

void AmpCircuitHopReach::NoteInflightTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  std::lock_guard lock(inflight_mu_);
  if (std::find(inflight_tunnels_.begin(), inflight_tunnels_.end(), id.value) == inflight_tunnels_.end()) {
    inflight_tunnels_.push_back(id.value);
  }
}

void AmpCircuitHopReach::ClearInflightTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  std::lock_guard lock(inflight_mu_);
  std::erase(inflight_tunnels_, id.value);
}

std::vector<CircuitTunnelId> AmpCircuitHopReach::TakeInflightTunnels() {
  std::vector<uint64_t> values;
  {
    std::lock_guard lock(inflight_mu_);
    values.swap(inflight_tunnels_);
  }
  std::vector<CircuitTunnelId> ids;
  ids.reserve(values.size());
  for (const uint64_t value : values) {
    ids.push_back(CircuitTunnelId{value});
  }
  return ids;
}

void AmpCircuitHopReach::AbortPending() {
  deferred_.Invalidate();
  const std::vector<CircuitTunnelId> ids = TakeInflightTunnels();
  for (const CircuitTunnelId id : ids) {
    circuit_.CancelTunnel(id);
  }
  AmpReachLog().info << "AbortPending gen=" << deferred_.Snapshot() << " cancelled_tunnels=" << ids.size();
}

CircuitReachRoe AmpCircuitHopReach::TryEnsureHopReachable(const std::string& hop_peer_id) {
  if (stopping_ && stopping_()) {
    AmpReachLog().debug << "TryEnsureHopReachable rejected: shutting down";
    return Failure::Of(Err::Unavailable, "shutdown in progress");
  }
  SettledWait<void, Failure> wait;
  TryEnsureHopReachableAsync(hop_peer_id, [wait](CircuitReachRoe value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "circuit hop reach timed out"));
}

CircuitReachRoe AmpCircuitHopReach::TryEnsurePeerReachable(const std::string& peer_key) {
  if (stopping_ && stopping_()) {
    AmpReachLog().debug << "TryEnsurePeerReachable rejected: shutting down";
    return Failure::Of(Err::Unavailable, "shutdown in progress");
  }
  SettledWait<void, Failure> wait;
  TryEnsurePeerReachableAsync(peer_key, [wait](CircuitReachRoe value) { wait.Finish(std::move(value)); });
  const auto deadline = Clock::now() + std::chrono::milliseconds(30000);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "call-media circuit reach timed out"));
}

void AmpCircuitHopReach::TryUpgradeToDirectAsync(const std::string& peer_key, CircuitReachDone on_done) {
  if (!on_done) {
    return;
  }
  if (peer_key.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing upgrade peer"));
    return;
  }
  if (!try_punch_via_introducer_) {
    on_done(Failure::Of(Err::Unavailable, "circuit upgrade punch unavailable"));
    return;
  }
  std::optional<AmpCircuitHopRegistry::Hop> hop = hops_.Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId);
  if (!hop) {
    hop = hops_.Find(peer_key, kMediaRelayProtocolId);
  }
  if (!hop) {
    on_done(Failure::Of(Err::NoCircuit, "no circuit hop to upgrade"));
    return;
  }
  // The circuit's relay introduces the punch. The circuit itself is left alone: the call moves
  // onto the direct link make-before-break (call-path-resilience k3) and releases its relayed path.
  try_punch_via_introducer_(
      hop->relay_peer_key, peer_key,
      [this, peer_key, on_done = std::move(on_done)](PunchIntroducerWalk::WalkRoe punched) mutable {
        if (!punched) {
          on_done(WrapPunchFailure(punched.error()));
          return;
        }
        const auto snap = links_.SnapshotByPeerId(peer_key);
        if (snap.base.phase != pp::amp::PeerLinkPhase::Connected || snap.base.carrier_backed) {
          on_done(Failure::Of(Err::NotConnected, "upgrade punch did not yield a direct link"));
          return;
        }
        on_done(CircuitReachRoe());
      });
}

} // namespace pp::mesh
