#include "domain/mesh/l4/circuit/serve/CircuitRelayServer.h"

#include "domain/mesh/l4/circuit/CircuitBridgeTarget.h"
#include "common/metrics/MetricsRegistry.h"
#include "domain/mesh/l4/circuit/CircuitBundleLogic.h"
#include "domain/mesh/l4/circuit/CircuitChannelPolicy.h"
#include "domain/mesh/l4/circuit/CircuitServeDialPolicy.h"

#include "amp/L3/ChannelBridge.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "common/ValueJson.h"
#include "common/Logger.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "foundation/runtime/DeferredSelf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

logging::Logger& CircuitTunnelLog() {
  static logging::Logger log = logging::getLogger("CircuitTunnel");
  return log;
}

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

std::vector<uint8_t> ErrorBody(const std::string& message) {
  Object err;
  err.set("v", int64_t{1});
  err.set("ok", false);
  err.set("error", message);
  return JsonToBody(DumpJson(err));
}

Roe<std::pair<std::string, std::string>> NormalizeAmpCircuitTarget(pp::amp::PeerLinkManager& links,
                                                                   const CircuitBridgeTarget& target) {
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    return Error("missing circuit bridge target");
  }
  if (!target.target_multiaddr.empty()) {
    auto parsed = pp::amp::ParseAdpMultiaddr(target.target_multiaddr);
    if (!parsed) {
      return parsed.error();
    }
    const std::string peer_id = !target.target_peer_id.empty() ? target.target_peer_id : parsed->peer_id;
    if (peer_id.empty()) {
      return Error("circuit target multiaddr missing peer id");
    }
    if (auto registered = links.RegisterEndpoint(peer_id, target.target_multiaddr); !registered) {
      return registered.error();
    }
    return std::make_pair(peer_id, target.target_multiaddr);
  }
  // Peer-id-only (nested call-media): H010 — live Connected only (CircuitServeDialPolicy).
  if (CircuitPeerIdOnlyHasLiveFarLeg(links.CountConnectedLinksForPeerId(target.target_peer_id))) {
    return std::make_pair(target.target_peer_id, std::string{});
  }
  return Error(std::string(kCircuitTargetPeerNotRegistered));
}

int64_t SteadyMs(const Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

} // namespace

namespace {

/** Operator metrics (docs/contracts/NODE_METRICS.md § Circuit relay). */
struct CircuitRelayMetrics {
  MetricCounter& bridge_accepted;
  MetricCounter& bridge_refused_admission;
  MetricCounter& bridge_refused_standby_full;
  MetricCounter& reserve_accepted;
  MetricCounter& reserve_refused;
  MetricCounter& tunnels_bridged;
  MetricCounter& tunnels_failed;
  MetricHistogram& setup_seconds;

  static CircuitRelayMetrics& Get() {
    static CircuitRelayMetrics metrics = [] {
      MetricsRegistry& r = MetricsRegistry::Global();
      const char* requests = "Circuit relay requests, by op and result.";
      const char* tunnels = "Circuit relay tunnels ended setup, by result.";
      return CircuitRelayMetrics{
          r.Counter("pp_circuit_relay_requests_total", requests, {{"op", "bridge"}, {"result", "accepted"}}),
          r.Counter("pp_circuit_relay_requests_total", requests, {{"op", "bridge"}, {"result", "refused_admission"}}),
          r.Counter("pp_circuit_relay_requests_total", requests,
                    {{"op", "bridge"}, {"result", "refused_standby_full"}}),
          r.Counter("pp_circuit_relay_requests_total", requests, {{"op", "reserve"}, {"result", "accepted"}}),
          r.Counter("pp_circuit_relay_requests_total", requests, {{"op", "reserve"}, {"result", "refused"}}),
          r.Counter("pp_circuit_relay_tunnels_total", tunnels, {{"result", "bridged"}}),
          r.Counter("pp_circuit_relay_tunnels_total", tunnels, {{"result", "failed"}}),
          r.Histogram("pp_circuit_relay_setup_seconds", "Circuit relay request to bridged.",
                      {0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10}),
      };
    }();
    return metrics;
  }
};

} // namespace

struct CircuitRelayServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  std::mutex mu;
  CircuitRelayAdmissionPolicy admission;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<bool> serve_inbound{true};
  std::atomic<uint64_t> next_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  pp::amp::PeerLinkManager::PeerConnectedListenerId peer_connected_listener_id_ = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  DeferredSelf deferred;
  /** IoTick / PeerConnected / protocol handler — Invalidate only on Stop (survives mid-life Abort). */
  DeferredSelf lifetime;

  /** One served bridge: dialer ↔ this relay (near) spliced to this relay ↔ target (far). */
  struct Tunnel {
    CircuitTunnelId id;
    CircuitTunnelPhase phase = CircuitTunnelPhase::Idle;
    Clock::time_point deadline{};
    /** Request received (setup latency); set once bridged. */
    Clock::time_point created = Clock::now();
    bool bridged = false;
    CircuitBridgeTarget target;
    std::string dialer_peer_id;
    std::string resolved_multiaddr;
    std::shared_ptr<pp::amp::ChannelSession> near_session;  // dialer↔relay circuit channel
    std::shared_ptr<pp::amp::ChannelSession> far_session;   // relay↔target protocol channel
    std::shared_ptr<pp::amp::ChannelBridge> bridge;
    bool finished = false;
    bool local_cancel = false;
    /** Absolute steady ms; 0 = not waiting for far PeerLink (event-driven ServeDial). */
    int64_t serve_far_wait_deadline_ms = 0;
  };

  struct Reservation {
    std::shared_ptr<pp::amp::ChannelSession> session;
    Clock::time_point deadline{};
  };

  std::unordered_map<uint64_t, std::unique_ptr<Tunnel>> tunnels;
  size_t max_standby = kCircuitDefaultMaxStandby;
  size_t max_standby_per_dialer = kCircuitDefaultMaxStandbyPerDialer;
  /** PeerId → parked inbound circuit channel from an answerer (op=reserve). */
  std::unordered_map<std::string, Reservation> reservations;
  /** PeerId → ServeDial tunnels waiting for Connected / reserve (H010 event wait). */
  std::unordered_map<std::string, std::vector<CircuitTunnelId>> far_leg_waiters_;

  /** Live standby circuits this relay serves (K003), relay-wide and from `dialer`. Under `mu`. */
  void CountStandbyLocked(const std::string& dialer, CircuitAdmitContext& admit) const {
    admit.max_standby = max_standby;
    admit.max_standby_per_dialer = max_standby_per_dialer;
    for (const auto& [_, tunnel] : tunnels) {
      if (!tunnel || tunnel->finished || tunnel->target.standby_priority == CircuitStandbyPriority::None) {
        continue;
      }
      ++admit.standby_total;
      if (tunnel->dialer_peer_id == dialer) {
        ++admit.standby_from_dialer;
      }
    }
  }

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    // Exclusive deferred-self post: Invalidate on Abort/Stop makes queued raw-this work no-op.
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); }, std::move(task));
  }

  Tunnel* Find(const CircuitTunnelId id) {
    auto it = tunnels.find(id.value);
    return it == tunnels.end() ? nullptr : it->second.get();
  }

  void ClearFarLegWait(Tunnel& tunnel) {
    tunnel.serve_far_wait_deadline_ms = 0;
    const std::string& peer_id = tunnel.target.target_peer_id;
    if (peer_id.empty()) {
      return;
    }
    auto it = far_leg_waiters_.find(peer_id);
    if (it == far_leg_waiters_.end()) {
      return;
    }
    auto& ids = it->second;
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const CircuitTunnelId id) { return id.value == tunnel.id.value; }),
              ids.end());
    if (ids.empty()) {
      far_leg_waiters_.erase(it);
    }
  }

  void ArmFarLegWait(Tunnel& tunnel) {
    const int64_t tunnel_deadline_ms =
        tunnel.deadline.time_since_epoch().count() == 0 ? 0 : SteadyMs(tunnel.deadline);
    tunnel.serve_far_wait_deadline_ms = CircuitServeDialArmFarLegWaitDeadlineMs(SteadyMs(Clock::now()), tunnel_deadline_ms);
    tunnel.phase = CircuitTunnelPhase::ServeDial;
    const std::string& peer_id = tunnel.target.target_peer_id;
    if (peer_id.empty()) {
      return;
    }
    auto& ids = far_leg_waiters_[peer_id];
    for (const auto id : ids) {
      if (id.value == tunnel.id.value) {
        return;
      }
    }
    ids.push_back(tunnel.id);
  }

  /** PeerConnected / reserve event — resume ServeDial waiters for this PeerId. */
  void NotifyFarLegReady(const std::string& peer_id) {
    if (peer_id.empty() || !runtime) {
      return;
    }
    std::vector<CircuitTunnelId> ids;
    {
      std::lock_guard lock(mu);
      auto it = far_leg_waiters_.find(peer_id);
      if (it == far_leg_waiters_.end()) {
        return;
      }
      ids = it->second;
    }
    for (const auto id : ids) {
      PostIo([this, id]() {
        Tunnel* tunnel = nullptr;
        {
          std::lock_guard lock(mu);
          tunnel = Find(id);
          if (!tunnel || tunnel->finished || tunnel->phase != CircuitTunnelPhase::ServeDial ||
              tunnel->serve_far_wait_deadline_ms == 0) {
            return;
          }
        }
        BeginServe(*tunnel);
      });
    }
  }

  void ScheduleWhenChannelOpen(std::string peer_key, const uint32_t channel_id, const Clock::time_point deadline,
                               std::function<void(bool open)> done) {
    if (!done) {
      return;
    }
    if (stopped.load(std::memory_order_acquire) || !runtime || peer_key.empty()) {
      done(false);
      return;
    }
    AmpWhenChannelOpen(runtime->Links(), peer_key, channel_id, deadline, std::move(done));
  }

  pp::amp::PeerLink* ResolveLink(const std::string& peer_key) const {
    if (!runtime || peer_key.empty()) {
      return nullptr;
    }
    return runtime->Links().FindLink(peer_key);
  }

  /**
   * PeerLink drop leaves ChannelSession mux_ dangling — tear down before CloseQuiet.
   * Do not treat FindLink(key)==null alone as lost: ADP links are often under inbound:…
   * or dial aliases while the tunnel key is PeerId ([A024] / dogfood dual-NAT). Grace while a
   * dialable endpoint remains during ServeDial.
   */
  bool PeerLinkMissing(const Tunnel& tunnel) const {
    if (!runtime) {
      return false;
    }
    auto adp_gone = [this](const std::string& peer_id) {
      if (peer_id.empty()) {
        return false;
      }
      if (runtime->Links().CountConnectedLinksForPeerId(peer_id) > 0) {
        return false;
      }
      return runtime->Links().FindLink(peer_id) == nullptr;
    };
    auto dialing_grace = [this](const CircuitTunnelPhase phase, const std::string& peer_id) {
      if (peer_id.empty() || phase != CircuitTunnelPhase::ServeDial) {
        return false;
      }
      return runtime->Links().GetLinkSnapshot(peer_id).has_endpoint;
    };
    if (tunnel.near_session && adp_gone(tunnel.dialer_peer_id) &&
        !dialing_grace(tunnel.phase, tunnel.dialer_peer_id)) {
      return true;
    }
    if (tunnel.far_session && !tunnel.target.target_peer_id.empty() && adp_gone(tunnel.target.target_peer_id) &&
        !dialing_grace(tunnel.phase, tunnel.target.target_peer_id)) {
      return true;
    }
    return false;
  }

  /** Requires `mu`. The dialer hears why (never an opaque timeout), then the tunnel goes. */
  void FailNear(Tunnel& tunnel, const std::string& message) {
    if (tunnel.near_session) {
      tunnel.near_session->EnqueueOutbound(ErrorBody(message));
    }
    TearDown(tunnel, false, message);
  }

  /** Requires `mu`. */
  void TearDown(Tunnel& tunnel, const bool local_cancel, const std::string& /*error*/) {
    if (!tunnel.finished && !tunnel.bridged) {
      CircuitRelayMetrics::Get().tunnels_failed.Inc();
    }
    ClearFarLegWait(tunnel);
    tunnel.local_cancel = local_cancel || tunnel.local_cancel;
    tunnel.phase = CircuitTunnelPhase::Closing;
    tunnel.finished = true;
    if (tunnel.bridge) {
      auto bridge = std::move(tunnel.bridge);
      bridge->Stop();
    }
    if (tunnel.near_session) {
      CloseQuietSlot(tunnel.near_session, ResolveLink(tunnel.dialer_peer_id));
    }
    if (tunnel.far_session) {
      CloseQuietSlot(tunnel.far_session, ResolveLink(tunnel.target.target_peer_id));
    }
    tunnels.erase(tunnel.id.value);
  }

  void TickDeadlines() {
    const auto now = Clock::now();
    const int64_t now_ms = SteadyMs(now);
    std::vector<CircuitTunnelId> timed_out;
    std::vector<CircuitTunnelId> link_lost;
    std::vector<CircuitTunnelId> far_wait_expired;
    std::vector<std::string> expired_reserves;
    {
      std::lock_guard lock(mu);
      for (auto& [_, tunnel] : tunnels) {
        if (!tunnel || tunnel->finished || tunnel->phase == CircuitTunnelPhase::Closing) {
          continue;
        }
        if (PeerLinkMissing(*tunnel)) {
          link_lost.push_back(tunnel->id);
          continue;
        }
        // Deadline event for event-driven ServeDial far-leg wait (not a poll-resume).
        if (tunnel->phase == CircuitTunnelPhase::ServeDial && tunnel->serve_far_wait_deadline_ms != 0 &&
            now_ms >= tunnel->serve_far_wait_deadline_ms) {
          far_wait_expired.push_back(tunnel->id);
          continue;
        }
        if (tunnel->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= tunnel->deadline && CircuitTunnelPhaseIsActive(tunnel->phase) &&
            tunnel->phase != CircuitTunnelPhase::Bridging) {
          timed_out.push_back(tunnel->id);
        }
      }
      for (auto& [peer_id, res] : reservations) {
        if (now >= res.deadline || !res.session || res.session->IsClosed()) {
          expired_reserves.push_back(peer_id);
        }
      }
    }
    for (const auto id : link_lost) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id); tunnel && tunnel->phase != CircuitTunnelPhase::Closing) {
        TearDown(*tunnel, false, "circuit-relay peer link lost");
      }
    }
    for (const auto id : far_wait_expired) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id); tunnel && !tunnel->finished && tunnel->phase == CircuitTunnelPhase::ServeDial) {
        FailNear(*tunnel, std::string(kCircuitTargetPeerNotRegistered));
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id)) {
        // ServeDial must always fail_near before TearDown — otherwise dialer WaitAck expires as
        // opaque `bridge timed out` (dogfood c44e34) and H010 sticky cannot classify the miss.
        const bool serve_dial = tunnel->phase == CircuitTunnelPhase::ServeDial;
        const bool far_wait = serve_dial && tunnel->serve_far_wait_deadline_ms != 0;
        const bool peer_id_only = tunnel->target.target_multiaddr.empty() && !tunnel->target.target_peer_id.empty();
        const std::string err_msg = far_wait || (serve_dial && peer_id_only)
                                        ? std::string(kCircuitTargetPeerNotRegistered)
                                        : (serve_dial ? "relay target stream timed out" : "circuit-relay bridge timed out");
        if (serve_dial) {
          FailNear(*tunnel, err_msg);
        } else {
          TearDown(*tunnel, false, err_msg);
        }
      }
    }
    for (const auto& peer_id : expired_reserves) {
      std::lock_guard lock(mu);
      auto it = reservations.find(peer_id);
      if (it == reservations.end()) {
        continue;
      }
      CloseQuietSlot(it->second.session, ResolveLink(peer_id));
      reservations.erase(it);
    }
  }

  void ArmBridge(Tunnel& tunnel) {
    if (!tunnel.near_session || !tunnel.far_session) {
      TearDown(tunnel, false, "circuit-relay bridge missing sessions");
      return;
    }
    tunnel.phase = CircuitTunnelPhase::Bridging;
    tunnel.bridged = true;
    CircuitRelayMetrics& metrics = CircuitRelayMetrics::Get();
    metrics.tunnels_bridged.Inc();
    metrics.setup_seconds.Observe(std::chrono::duration<double>(Clock::now() - tunnel.created).count());
    tunnel.bridge = std::make_shared<pp::amp::ChannelBridge>();
    const CircuitTunnelId id = tunnel.id;
    tunnel.bridge->Attach(tunnel.near_session, tunnel.far_session, {}, [this, id]() {
      PostIo([this, id]() {
        std::lock_guard lock(mu);
        if (auto* tunnel = Find(id)) {
          const auto decision = DecideCircuitTunnelClose(CircuitTunnelCloseContext{
              .phase = tunnel->phase,
              .local_cancel = tunnel->local_cancel,
              .remote_terminal = true,
              .finished = tunnel->finished,
          });
          if (decision == CircuitTunnelCloseDecision::Ignore) {
            return;
          }
          TearDown(*tunnel, tunnel->local_cancel, "circuit-relay tunnel closed");
        }
      });
    });
  }

  void ContinueServeAfterTargetOpen(Tunnel& tunnel, pp::amp::PeerLink& target_link, const uint32_t channel_id) {
    tunnel.far_session = std::make_shared<pp::amp::ChannelSession>();
    tunnel.far_session->Bind(*target_link.Mux(), channel_id, CircuitTargetChannelPolicy(tunnel.target.target_protocol),
                             [](Roe<std::vector<uint8_t>>) { return true; });

    Object response;
    response.set("v", int64_t{1});
    response.set("ok", true);
    response.set("resolved_multiaddr", tunnel.resolved_multiaddr);
    if (!tunnel.near_session->EnqueueOutbound(JsonToBody(DumpJson(response)))) {
      TearDown(tunnel, false, "failed to ack bridge");
      return;
    }
    ArmBridge(tunnel);
  }

  /** Far leg channel requested on `dial_key`: wait for it to open, then splice. */
  void OnFarChannel(const CircuitTunnelId id, const std::string& dial_key, const Clock::time_point deadline,
                    pp::amp::PeerLinkManager::ChannelRoe channel) {
    uint32_t channel_id = 0;
    {
      std::lock_guard lock(mu);
      auto* tunnel = Find(id);
      if (!tunnel) {
        return;
      }
      if (!channel) {
        FailNear(*tunnel, channel.error().message);
        return;
      }
      if (!runtime->Links().FindLink(dial_key)) {
        TearDown(*tunnel, false, "relay target stream timed out");
        return;
      }
      channel_id = *channel;
    }
    ScheduleWhenChannelOpen(dial_key, channel_id, deadline, [this, id, dial_key, channel_id](const bool open) {
      std::lock_guard lock(mu);
      auto* tunnel = Find(id);
      if (!tunnel) {
        return;
      }
      if (!open) {
        FailNear(*tunnel, "relay target stream timed out");
        return;
      }
      auto* link = runtime->Links().FindLink(dial_key);
      if (!link || !link->Mux()) {
        TearDown(*tunnel, false, "relay target stream timed out");
        return;
      }
      ContinueServeAfterTargetOpen(*tunnel, *link, channel_id);
    });
  }

  CircuitAdmitDecision AdmitBridge(const Tunnel& tunnel) {
    CircuitAdmitContext admit;
    admit.service_started = started.load(std::memory_order_acquire) && serve_inbound.load(std::memory_order_acquire);
    admit.stopping = stopped.load(std::memory_order_acquire);
    admit.dialer_peer_id = tunnel.dialer_peer_id;
    admit.op = "bridge";
    {
      std::lock_guard lock(mu);
      admit.serve_scope_mask = admission.serve_scope_mask;
      admit.contact_peer_ids = admission.contact_peer_ids;
    }
    return DecideCircuitAdmit(admit);
  }

  /**
   * Peer-id-only target without a live far leg: wait for PeerConnected / reserve (H010), not a
   * Tick poll. Returns true when BeginServe should stop here (waiting or failed).
   */
  bool WaitForFarLeg(Tunnel& tunnel) {
    const size_t connected = runtime->Links().CountConnectedLinksForPeerId(tunnel.target.target_peer_id);
    if (CircuitPeerIdOnlyHasLiveFarLeg(connected)) {
      ClearFarLegWait(tunnel);
      return false;
    }
    if (tunnel.serve_far_wait_deadline_ms != 0 && SteadyMs(Clock::now()) >= tunnel.serve_far_wait_deadline_ms) {
      FailNear(tunnel, std::string(kCircuitTargetPeerNotRegistered));
      return true;
    }
    ArmFarLegWait(tunnel);
    // Lost-wakeup: PeerConnected / reserve may land between CountConnected and arm.
    if (!CircuitPeerIdOnlyHasLiveFarLeg(runtime->Links().CountConnectedLinksForPeerId(tunnel.target.target_peer_id))) {
      return true;
    }
    ClearFarLegWait(tunnel);
    return false;
  }

  void BeginServe(Tunnel& tunnel) {
    tunnel.phase = CircuitTunnelPhase::ServeDial;
    const auto decision = AdmitBridge(tunnel);
    if (decision == CircuitAdmitDecision::RefuseStranger) {
      FailNear(tunnel, "relay scope: stranger refused");
      return;
    }
    if (decision != CircuitAdmitDecision::Allow) {
      FailNear(tunnel, decision == CircuitAdmitDecision::RefuseBadOp ? "unsupported op"
                                                                     : "circuit-relay service not ready");
      return;
    }

    // Live op=reserve → peer-id-only Connected path (H010 CircuitServeDialPolicy).
    bool reservation_hit = false;
    {
      std::lock_guard lock(mu);
      const std::string& tid = tunnel.target.target_peer_id;
      if (!tid.empty()) {
        auto it = reservations.find(tid);
        const bool live_res = it != reservations.end() && it->second.session && !it->second.session->IsClosed();
        reservation_hit = live_res;
        if (CircuitServeDialClearTargetMaWhenReserved(live_res, !tunnel.target.target_multiaddr.empty())) {
          tunnel.target.target_multiaddr.clear();
        }
      }
    }

    const bool peer_id_only = tunnel.target.target_multiaddr.empty() && !tunnel.target.target_peer_id.empty();
    const size_t connected_for_target =
        peer_id_only ? runtime->Links().CountConnectedLinksForPeerId(tunnel.target.target_peer_id) : 0;
    CircuitTunnelLog().info << "circuit ServeDial dialer=" << tunnel.dialer_peer_id
                            << " serve_target=" << tunnel.target.target_peer_id
                            << " peer_id_only=" << (peer_id_only ? 1 : 0)
                            << " reservation_hit=" << (reservation_hit ? 1 : 0)
                            << " connected_for_target=" << connected_for_target;
    if (peer_id_only && WaitForFarLeg(tunnel)) {
      return;
    }

    auto normalized = NormalizeAmpCircuitTarget(runtime->Links(), tunnel.target);
    if (!normalized) {
      FailNear(tunnel, normalized.error().message);
      return;
    }
    tunnel.resolved_multiaddr = normalized->second;
    const std::string target_key = normalized->first;
    const CircuitTunnelId id = tunnel.id;
    const auto deadline = tunnel.deadline;
    const std::string target_protocol = tunnel.target.target_protocol;

    // Peer-id-only → OpenChannelOnLink on live far leg (H010); never EnsureAssociation→Preferred.
    if (CircuitServeDialOpenOnLiveLink(normalized->second.empty())) {
      auto* live = runtime->Links().FindLinkByPeerId(target_key);
      if (!live || live->Phase() != pp::amp::PeerLinkPhase::Connected || !live->Mux()) {
        FailNear(tunnel, std::string(kCircuitTargetPeerNotRegistered));
        return;
      }
      const std::string dial_key = live->PeerKey();
      runtime->Links().OpenChannelOnLink(
          *live, target_protocol, CircuitTargetChannelPolicy(target_protocol),
          [this, id, dial_key, deadline](pp::amp::PeerLinkManager::ChannelRoe channel) {
            OnFarChannel(id, dial_key, deadline, std::move(channel));
          });
      return;
    }

    runtime->Links().EnsureAssociation(
        target_key, [this, id, target_key, deadline, target_protocol](pp::amp::PeerLinkManager::LinkRoe assoc) {
          {
            std::lock_guard lock(mu);
            auto* tunnel = Find(id);
            if (!tunnel) {
              return;
            }
            if (!assoc) {
              FailNear(*tunnel, assoc.error().message);
              return;
            }
          }
          runtime->Links().OpenChannel(
              target_key, target_protocol, CircuitTargetChannelPolicy(target_protocol),
              [this, id, target_key, deadline](pp::amp::PeerLinkManager::ChannelRoe channel) {
                OnFarChannel(id, target_key, deadline, std::move(channel));
              });
        });
  }

  /** Off-IO continuation of an inbound request (op=reserve or op=bridge). */
  void HandleRequest(const std::shared_ptr<pp::amp::ChannelSession>& near_session, const std::string& remote,
                     const Object& root) {
    const std::string op = root.getString("op").value_or("");
    CircuitAdmitContext admit;
    admit.service_started = started.load(std::memory_order_acquire) && serve_inbound.load(std::memory_order_acquire);
    admit.stopping = stopped.load(std::memory_order_acquire);
    admit.dialer_peer_id = remote;
    admit.op = op;
    admit.standby_priority = ParseCircuitStandbyPriority(root.getString("standby_priority").value_or(""));
    {
      std::lock_guard lock(mu);
      admit.serve_scope_mask = admission.serve_scope_mask;
      admit.contact_peer_ids = admission.contact_peer_ids;
      CountStandbyLocked(remote, admit);
    }
    const auto decision = DecideCircuitAdmit(admit);
    auto refuse = [&](const std::string& message) {
      near_session->EnqueueOutbound(ErrorBody(message));
      near_session->Close();
    };
    CircuitRelayMetrics& metrics = CircuitRelayMetrics::Get();
    if (decision == CircuitAdmitDecision::RefuseStandbyFull) {
      metrics.bridge_refused_standby_full.Inc();
      CircuitTunnelLog().info << "circuit standby refused dialer=" << remote
                              << " priority=" << CircuitStandbyPriorityWire(admit.standby_priority)
                              << " standby=" << admit.standby_total;
      refuse("relay busy: standby refused");
      return;
    }
    if (decision != CircuitAdmitDecision::Allow) {
      (op == "reserve" ? metrics.reserve_refused : metrics.bridge_refused_admission).Inc();
      refuse(decision == CircuitAdmitDecision::RefuseStranger
                 ? "relay scope: stranger refused"
                 : (decision == CircuitAdmitDecision::RefuseBadOp ? "unsupported op" : "circuit-relay service not ready"));
      return;
    }
    if (op == "reserve") {
      HandleReserve(near_session, remote, root, refuse);
      return;
    }
    metrics.bridge_accepted.Inc();
    CircuitTunnelId id{};
    {
      std::lock_guard lock(mu);
      auto tunnel = std::make_unique<Tunnel>();
      tunnel->id = CircuitTunnelId{next_id.fetch_add(1, std::memory_order_relaxed)};
      tunnel->dialer_peer_id = remote;
      tunnel->near_session = near_session;
      tunnel->target.target_peer_id = root.getString("target_peer_id").value_or("");
      tunnel->target.target_multiaddr = root.getString("target_multiaddr").value_or("");
      tunnel->target.target_protocol = root.getString("target_protocol").value_or("");
      tunnel->target.standby_priority = admit.standby_priority;
      if (tunnel->target.target_protocol.empty()) {
        tunnel->target.target_protocol = kCircuitRelayProtocolId;
      }
      // The near leg was bound Control (Reliable, strict in-order) to read this JSON request. A
      // call-media carrier is sent best-effort by the dialer: keep Control and the first lost /
      // reordered frame makes the mux reject every later one ("out of order seq") — dialer→target
      // dead for the rest of the call (hard lab CGNAT + 1 % loss; dogfood 2026-09-24 16:17). Match
      // the far leg's policy.
      if (auto* mux = near_session->Mux()) {
        (void)mux->ApplyChannelPolicy(near_session->ChannelId(),
                                      CircuitTargetChannelPolicy(tunnel->target.target_protocol));
      }
      const int timeout_ms = static_cast<int>(root.getNonNegInt("timeout_ms").value_or(8000));
      tunnel->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      id = tunnel->id;
      tunnels[tunnel->id.value] = std::move(tunnel);
    }
    if (auto* tunnel = Find(id)) {
      BeginServe(*tunnel);
    }
  }

  template <typename Refuse>
  void HandleReserve(const std::shared_ptr<pp::amp::ChannelSession>& near_session, const std::string& remote,
                     const Object& root, Refuse& refuse) {
    if (remote.empty()) {
      CircuitRelayMetrics::Get().reserve_refused.Inc();
      CircuitTunnelLog().warning << "circuit reserve refused: remote peer id unknown";
      refuse("circuit reserve: remote peer id unknown");
      return;
    }
    const int timeout_ms = static_cast<int>(root.getNonNegInt("timeout_ms").value_or(30000));
    {
      std::lock_guard lock(mu);
      Reservation res;
      res.session = near_session;
      res.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
      reservations[remote] = std::move(res);
    }
    CircuitRelayMetrics::Get().reserve_accepted.Inc();
    CircuitTunnelLog().info << "circuit reserve key=" << remote << " timeout_ms=" << timeout_ms;
    Object ack;
    ack.set("v", int64_t{1});
    ack.set("ok", true);
    ack.set("op", "reserve");
    near_session->EnqueueOutbound(JsonToBody(DumpJson(ack)));
    // Answerer park may complete after the dialer already armed a ServeDial wait.
    NotifyFarLegReady(remote);
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id, const std::string& handler_peer_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !link.Mux()) {
      return;
    }
    auto near_session = std::make_shared<pp::amp::ChannelSession>();
    auto started_req = std::make_shared<std::atomic<bool>>(false);
    // Prefer protocol-handler PeerId (authenticated) over link.RemotePeerId() which can be
    // empty mid-handshake — empty reserve key breaks ServeDial lookup (B27).
    std::string remote = !handler_peer_id.empty() ? handler_peer_id : link.RemotePeerId();
    if (remote.empty()) {
      remote = link.PeerKey();
    }
    near_session->Bind(*link.Mux(), channel_id, pp::amp::CircuitTunnelChannelPolicy(),
                       [this, near_session, started_req, remote](Roe<std::vector<uint8_t>> frame) {
                         if (!frame || stopped.load(std::memory_order_acquire)) {
                           return false;
                         }
                         if (started_req->exchange(true, std::memory_order_acq_rel)) {
                           return true;  // superseded once tunnel Bind/Bridge takes over
                         }
                         auto root = TryParseObject(BodyToJson(*frame));
                         if (!root) {
                           near_session->EnqueueOutbound(ErrorBody("invalid circuit-relay json"));
                           return false;
                         }
                         PostIo([this, near_session, remote, root = *root]() { HandleRequest(near_session, remote, root); });
                         return true;
                       });
  }
};

CircuitRelayServer::CircuitRelayServer(pp::amp::MeshRuntime& runtime)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  (void)CircuitRelayMetrics::Get();  // its series exist (at 0) from the start
  impl_->runtime = &runtime_;
}

CircuitRelayServer::~CircuitRelayServer() {
  Stop();
}

void CircuitRelayServer::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick(impl_->lifetime.Bind([impl = impl_.get()] { impl->TickDeadlines(); }));
  impl_->peer_connected_listener_id_ = runtime_.Links().AddPeerConnectedListener(
      impl_->lifetime.Bind([impl = impl_.get()](const std::string& peer_id) { impl->NotifyFarLegReady(peer_id); }));
  runtime_.Links().SetProtocolHandler(
      kCircuitRelayProtocolId,
      impl_->lifetime.Bind([impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                                const uint32_t ch) {
        if (!impl->runtime) {
          return;
        }
        impl->runtime->Links().WithLiveLink(
            handle, [&](pp::amp::PeerLink& link) { impl->HandleInboundChannel(link, ch, remote_peer_id); });
      }));
}

void CircuitRelayServer::Stop() {
  if (!impl_->started.load(std::memory_order_acquire) && impl_->stopped.load(std::memory_order_acquire)) {
    return;
  }
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  if (impl_->peer_connected_listener_id_ != 0) {
    runtime_.Links().RemovePeerConnectedListener(impl_->peer_connected_listener_id_);
    impl_->peer_connected_listener_id_ = 0;
  }
  runtime_.Links().RemoveProtocolHandler(kCircuitRelayProtocolId);
  AbortInflight();
  impl_->lifetime.Invalidate();
}

bool CircuitRelayServer::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void CircuitRelayServer::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->admission = std::move(policy);
}

void CircuitRelayServer::SetServeInbound(const bool serve) {
  impl_->serve_inbound.store(serve, std::memory_order_release);
}

bool CircuitRelayServer::ServeInbound() const {
  return impl_->serve_inbound.load(std::memory_order_acquire);
}

CircuitRelayRuntimeStats CircuitRelayServer::RuntimeStats() const {
  CircuitRelayRuntimeStats stats;
  std::lock_guard lock(impl_->mu);
  for (const auto& [id, tunnel] : impl_->tunnels) {
    if (!tunnel || tunnel->finished) {
      continue;
    }
    if (tunnel->phase == CircuitTunnelPhase::Bridging) {
      ++stats.active_bridges;
    } else {
      ++stats.pending_tunnels;
    }
  }
  stats.reservations = impl_->reservations.size();
  return stats;
}

void CircuitRelayServer::SetStandbyLimits(const size_t max_standby, const size_t max_per_dialer) {
  std::lock_guard lock(impl_->mu);
  if (max_standby > 0) {
    impl_->max_standby = max_standby;
  }
  if (max_per_dialer > 0) {
    impl_->max_standby_per_dialer = max_per_dialer;
  }
}

void CircuitRelayServer::AbortInflight() {
  // Strand before mu (IO callbacks hold the strand); off-IO callers (quit / Leave) take the strand
  // first — mu → strand deadlocked against MeshPump's TickDeadlines (strand → mu), 2026-09-25.
  runtime_.WithIoLock([this]() {
    std::lock_guard lock(impl_->mu);
    std::vector<uint64_t> ids;
    for (auto& [id, _] : impl_->tunnels) {
      ids.push_back(id);
    }
    for (const auto id : ids) {
      if (auto* tunnel = impl_->Find(CircuitTunnelId{id})) {
        impl_->TearDown(*tunnel, true, "circuit-relay aborted");
      }
    }
    for (auto& [peer_id, res] : impl_->reservations) {
      CloseQuietSlot(res.session, impl_->ResolveLink(peer_id));
    }
    impl_->reservations.clear();
  });
  // Poison already-queued PostIo(self) work; new posts after this capture a fresh snap.
  impl_->deferred.Invalidate();
}

} // namespace pbr
