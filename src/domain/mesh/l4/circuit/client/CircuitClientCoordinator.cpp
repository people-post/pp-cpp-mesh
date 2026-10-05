#include "domain/mesh/l4/circuit/client/CircuitClientCoordinator.h"

#include "domain/mesh/l4/circuit/CircuitChannelPolicy.h"

#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/link/PeerLink.h"
#include "common/ValueJson.h"
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

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

} // namespace

struct CircuitClientCoordinator::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  mutable std::mutex mu;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<uint64_t> next_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  DeferredSelf deferred;
  /** IoTick — Invalidate only on Stop (survives mid-life Abort). */
  DeferredSelf lifetime;

  /** One bridge or reserve request on a relay (the near channel is ours ↔ relay). */
  struct Tunnel {
    CircuitTunnelId id;
    CircuitTunnelPhase phase = CircuitTunnelPhase::Idle;
    Clock::time_point deadline{};
    CircuitBridgeTarget target;
    std::string relay_peer_key;
    std::string resolved_multiaddr;
    std::shared_ptr<pp::amp::ChannelSession> near_session;
    FrameHandler on_payload;
    ClosedCallback on_closed;
    BridgeFinished on_finished;
    bool finished = false;
    bool local_cancel = false;
    bool is_reserve = false;
  };

  std::unordered_map<uint64_t, std::unique_ptr<Tunnel>> tunnels;
  /**
   * Live Reserved tunnels per relay key. The relay never talks first, so a cold relay link idles
   * past the 5 s ADP liveness window and is evicted with the reservation (dogfood 2026-09-24
   * "Couldn't connect"). Hot while ≥ 1 reservation is held (K008).
   */
  std::unordered_map<std::string, int> reserved_relays;

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    // Exclusive deferred-self post: Invalidate on Abort/Stop makes queued raw-this work no-op.
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); }, std::move(task));
  }

  /** Finish / reserve notify: callback-only (no Impl*) — do not gate on deferred. */
  void PostFinishCb(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    runtime->PostToIo(std::move(task));
  }

  Tunnel* Find(const CircuitTunnelId id) {
    auto it = tunnels.find(id.value);
    return it == tunnels.end() ? nullptr : it->second.get();
  }

  const Tunnel* Find(const CircuitTunnelId id) const {
    auto it = tunnels.find(id.value);
    return it == tunnels.end() ? nullptr : it->second.get();
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
   * or dial aliases while the tunnel key is PeerId ([A024] / dogfood dual-NAT). Match
   * CallMediaLeg grace while a dialable endpoint remains during Open/WaitAck.
   */
  bool PeerLinkMissing(const Tunnel& tunnel) const {
    if (!runtime || !tunnel.near_session || tunnel.relay_peer_key.empty()) {
      return false;
    }
    const std::string& relay = tunnel.relay_peer_key;
    if (runtime->Links().CountConnectedLinksForPeerId(relay) > 0 || runtime->Links().FindLink(relay) != nullptr) {
      return false;
    }
    const bool dialing =
        tunnel.phase == CircuitTunnelPhase::OutboundOpen || tunnel.phase == CircuitTunnelPhase::WaitAck;
    return !(dialing && runtime->Links().GetLinkSnapshot(relay).has_endpoint);
  }

  void TickDeadlines() {
    const auto now = Clock::now();
    std::vector<CircuitTunnelId> timed_out;
    std::vector<CircuitTunnelId> link_lost;
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
        if (tunnel->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= tunnel->deadline &&
            (tunnel->phase == CircuitTunnelPhase::Reserved ||
             (CircuitTunnelPhaseIsActive(tunnel->phase) && tunnel->phase != CircuitTunnelPhase::Bridging))) {
          timed_out.push_back(tunnel->id);
        }
      }
    }
    for (const auto id : link_lost) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id); tunnel && tunnel->phase != CircuitTunnelPhase::Closing) {
        TearDown(*tunnel, false, false, "circuit-relay peer link lost");
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id)) {
        TearDown(*tunnel, false, false,
                 tunnel->is_reserve ? "circuit-relay reserve timed out" : "circuit-relay bridge timed out");
      }
    }
  }

  void Finish(Tunnel& tunnel, Roe<CircuitTunnelBridgeResult> result) {
    if (tunnel.finished) {
      return;
    }
    tunnel.finished = true;
    auto cb = std::move(tunnel.on_finished);
    tunnel.on_finished = nullptr;
    if (!cb) {
      return;
    }
    // Callers hold Impl::mu. try_relay → StartBridge → OpenChannel must not re-enter under lock
    // (dogfood hop give-up / 130521). Deliver on the IO queue after TearDown returns.
    PostFinishCb([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
  }

  void NoteReserveHeld(const std::string& relay_key) {
    if (++reserved_relays[relay_key] == 1) {
      runtime->Links().MarkHot(relay_key);
    }
  }

  void NoteReserveReleased(Tunnel& tunnel) {
    if (!tunnel.is_reserve || tunnel.phase != CircuitTunnelPhase::Reserved) {
      return;
    }
    auto it = reserved_relays.find(tunnel.relay_peer_key);
    if (it == reserved_relays.end()) {
      return;
    }
    if (--it->second <= 0) {
      reserved_relays.erase(it);
      runtime->Links().ClearWarm(tunnel.relay_peer_key);
    }
  }

  /**
   * Requires `mu`. A finished (bridged) tunnel only leaves the table: its session belongs to the
   * consumer that adopted it (AmpCircuitHopRegistry). Otherwise close the near channel and report.
   */
  void TearDown(Tunnel& tunnel, const bool suppress_notify, const bool local_cancel, const std::string& error) {
    NoteReserveReleased(tunnel);
    if (tunnel.finished) {
      tunnels.erase(tunnel.id.value);
      return;
    }
    tunnel.local_cancel = local_cancel || tunnel.local_cancel;
    tunnel.phase = CircuitTunnelPhase::Closing;
    if (tunnel.near_session) {
      CloseQuietSlot(tunnel.near_session, ResolveLink(tunnel.relay_peer_key));
    }
    const bool aborted = (suppress_notify || local_cancel) && error.empty();
    Finish(tunnel, Error(aborted ? "circuit-relay aborted" : error));
    tunnels.erase(tunnel.id.value);
  }

  /** Requires `mu`. The relay's ack to our bridge / reserve request. */
  bool HandleAck(Tunnel& tunnel, const std::vector<uint8_t>& frame) {
    auto root = TryParseObject(BodyToJson(frame));
    if (!root) {
      TearDown(tunnel, false, false, "invalid circuit-relay ack");
      return false;
    }
    const bool ack_ok = root->getIf<bool>("ok").value_or(false);
    const auto decision = DecideCircuitBridgeAck(CircuitBridgeAckContext{.phase = tunnel.phase, .ack_ok = ack_ok});
    if (decision == CircuitBridgeAckDecision::IgnoreStale) {
      return true;
    }
    if (decision == CircuitBridgeAckDecision::Fail) {
      TearDown(tunnel, false, false, root->getString("error").value_or("circuit-relay bridge refused"));
      return false;
    }
    tunnel.resolved_multiaddr = root->getString("resolved_multiaddr").value_or("");
    CircuitTunnelBridgeResult ok;
    ok.ok = true;
    ok.session = tunnel.near_session;
    if (tunnel.is_reserve) {
      tunnel.phase = CircuitTunnelPhase::Reserved;
      NoteReserveHeld(tunnel.relay_peer_key);
      // Notify once; keep tunnel + session until Cancel / TTL (do not mark finished).
      if (tunnel.on_finished) {
        auto cb = std::move(tunnel.on_finished);
        tunnel.on_finished = nullptr;
        PostFinishCb([cb = std::move(cb), ok = std::move(ok)]() mutable { cb(std::move(ok)); });
      }
      return true;
    }
    // The relay splices; locally the near channel is the whole tunnel (payload handler stays on
    // it, no ChannelBridge here).
    tunnel.phase = CircuitTunnelPhase::Bridging;
    ok.resolved_multiaddr = tunnel.resolved_multiaddr;
    Finish(tunnel, std::move(ok));
    return true;
  }

  void BindNear(Tunnel& tunnel, pp::amp::PeerLink& link, const uint32_t channel_id) {
    tunnel.near_session = std::make_shared<pp::amp::ChannelSession>();
    const CircuitTunnelId id = tunnel.id;
    tunnel.near_session->Bind(
        *link.Mux(), channel_id, CircuitTargetChannelPolicy(tunnel.target.target_protocol),
        [this, id](Roe<std::vector<uint8_t>> frame) {
          std::lock_guard lock(mu);
          auto* tunnel = Find(id);
          if (!tunnel) {
            return false;
          }
          if (!frame) {
            TearDown(*tunnel, false, false, "circuit-relay bridge failed");
            return false;
          }
          if (tunnel->phase == CircuitTunnelPhase::WaitAck) {
            return HandleAck(*tunnel, *frame);
          }
          if (tunnel->phase == CircuitTunnelPhase::Bridging && tunnel->on_payload) {
            return tunnel->on_payload(std::move(frame));
          }
          return true;
        },
        [this, id](const char* reason) {
          std::lock_guard lock(mu);
          auto* tunnel = Find(id);
          if (!tunnel) {
            return;
          }
          if (tunnel->on_closed) {
            tunnel->on_closed(reason);
          }
          if (tunnel->finished) {
            // Bridged (or a held reservation): nothing to report, but the record must go — the
            // close decision ignores finished tunnels, so each one stayed until Stop.
            TearDown(*tunnel, /*suppress_notify=*/true, /*local_cancel=*/false, "");
            return;
          }
          const auto decision = DecideCircuitTunnelClose(CircuitTunnelCloseContext{
              .phase = tunnel->phase,
              .local_cancel = tunnel->local_cancel,
              .remote_terminal = true,
              .finished = tunnel->finished,
          });
          if (decision == CircuitTunnelCloseDecision::Ignore) {
            return;
          }
          TearDown(*tunnel, decision == CircuitTunnelCloseDecision::SuppressNotify, tunnel->local_cancel,
                   "circuit-relay channel closed");
        });
  }

  std::string RequestJson(const Tunnel& tunnel) const {
    Object request;
    request.set("v", int64_t{1});
    request.set("op", tunnel.is_reserve ? "reserve" : "bridge");
    request.set("timeout_ms",
                int64_t{std::max<int64_t>(
                    1, std::chrono::duration_cast<std::chrono::milliseconds>(tunnel.deadline - Clock::now()).count())});
    if (!tunnel.is_reserve) {
      if (!tunnel.target.target_peer_id.empty()) {
        request.set("target_peer_id", tunnel.target.target_peer_id);
      }
      if (!tunnel.target.target_multiaddr.empty()) {
        request.set("target_multiaddr", tunnel.target.target_multiaddr);
      }
      request.set("target_protocol", tunnel.target.target_protocol);
      if (const char* standby = CircuitStandbyPriorityWire(tunnel.target.standby_priority)) {
        request.set("standby_priority", std::string(standby));
      }
    }
    return DumpJson(request);
  }

  void BeginOutbound(Tunnel& tunnel) {
    tunnel.phase = CircuitTunnelPhase::OutboundOpen;
    const CircuitTunnelId id = tunnel.id;
    const std::string relay_key = tunnel.relay_peer_key;
    const auto deadline = tunnel.deadline;
    const std::string request_json = RequestJson(tunnel);

    // Must not hold mu across OpenChannel — callback may run synchronously.
    runtime->Links().OpenChannel(
        relay_key, kCircuitRelayProtocolId,
        tunnel.is_reserve ? pp::amp::CircuitTunnelChannelPolicy() : CircuitTargetChannelPolicy(tunnel.target.target_protocol),
        [this, id, relay_key, deadline, request_json](pp::amp::PeerLinkManager::ChannelRoe channel) {
          uint32_t channel_id = 0;
          {
            std::lock_guard lock(mu);
            auto* tunnel = Find(id);
            if (!tunnel) {
              return;
            }
            if (!channel) {
              TearDown(*tunnel, false, false, channel.error().message);
              return;
            }
            if (!runtime->Links().FindLink(relay_key)) {
              TearDown(*tunnel, false, false, "amp circuit-relay: channel open failed");
              return;
            }
            channel_id = *channel;
          }
          ScheduleWhenChannelOpen(relay_key, channel_id, deadline, [this, id, relay_key, channel_id, request_json](const bool open) {
            std::shared_ptr<pp::amp::ChannelSession> near;
            {
              std::lock_guard lock(mu);
              auto* tunnel = Find(id);
              if (!tunnel) {
                return;
              }
              auto* link = open ? runtime->Links().FindLink(relay_key) : nullptr;
              if (!link || !link->Mux()) {
                TearDown(*tunnel, false, false, "amp circuit-relay: channel open failed");
                return;
              }
              BindNear(*tunnel, *link, channel_id);
              tunnel->phase = CircuitTunnelPhase::WaitAck;
              near = tunnel->near_session;
            }
            // Not under mu: a write that fails at once closes the channel inline, and its closed
            // callback (BindNear) takes mu — the I/O thread deadlocked on itself (hard-lab flip).
            if (!near->EnqueueOutbound(JsonToBody(request_json))) {
              std::lock_guard lock(mu);
              if (auto* tunnel = Find(id)) {
                TearDown(*tunnel, false, false, "failed to send circuit-relay bridge request");
              }
            }
          });
        });
  }

  /** Posts `on_finished(Error(why))` and returns true when a request cannot start. */
  bool RefuseStart(const std::string& relay_peer_key, BridgeFinished& on_finished) {
    const char* why = nullptr;
    if (!started.load(std::memory_order_acquire)) {
      why = "amp circuit-relay service not started";
    } else if (!runtime->Links().GetLinkSnapshot(relay_peer_key).has_endpoint) {
      why = "relay peer endpoint not registered";
    }
    if (!why) {
      return false;
    }
    if (on_finished) {
      runtime->PostToIo([on_finished = std::move(on_finished), why]() mutable { on_finished(Error(why)); });
    }
    return true;
  }

  CircuitTunnelId Launch(std::unique_ptr<Tunnel> tunnel) {
    const CircuitTunnelId id{next_id.fetch_add(1, std::memory_order_relaxed)};
    tunnel->id = id;
    PostIo([this, tunnel = std::shared_ptr<Tunnel>(std::move(tunnel))]() mutable {
      Tunnel* raw = nullptr;
      {
        std::lock_guard lock(mu);
        auto owned = std::make_unique<Tunnel>(std::move(*tunnel));
        raw = owned.get();
        tunnels[raw->id.value] = std::move(owned);
      }
      BeginOutbound(*raw);
    });
    return id;
  }
};

CircuitClientCoordinator::CircuitClientCoordinator(pp::amp::MeshRuntime& runtime)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

CircuitClientCoordinator::~CircuitClientCoordinator() {
  Stop();
}

void CircuitClientCoordinator::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick(impl_->lifetime.Bind([impl = impl_.get()] { impl->TickDeadlines(); }));
}

void CircuitClientCoordinator::Stop() {
  if (!impl_->started.load(std::memory_order_acquire) && impl_->stopped.load(std::memory_order_acquire)) {
    return;
  }
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  AbortInflight();
  impl_->lifetime.Invalidate();
}

bool CircuitClientCoordinator::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void CircuitClientCoordinator::AbortInflight() {
  // Null Finish cbs: CircuitRendezvousCoordinator / AmpCircuitHopReach may already be destroyed
  // (hard-w5 offerer SIGSEGV after Leave). Reach uses AbortPending gen; reserve cbs use
  // CircuitRendezvousCoordinator DeferredSelf.
  // Lock order is strand → mu (IO callbacks hold the strand). Off-IO callers (quit / Leave on the
  // UI thread) take the strand first: mu → ClearWarm (strand) deadlocked against MeshPump's
  // TickDeadlines (strand → mu) — pp-call-probe teardown hang, 2026-09-25.
  runtime_.WithIoLock([this]() {
    std::lock_guard lock(impl_->mu);
    std::vector<uint64_t> ids;
    for (auto& [id, _] : impl_->tunnels) {
      ids.push_back(id);
    }
    for (const auto id : ids) {
      if (auto* tunnel = impl_->Find(CircuitTunnelId{id})) {
        tunnel->on_finished = nullptr;
        impl_->TearDown(*tunnel, true, true, "circuit-relay aborted");
      }
    }
  });
  // Poison already-queued PostIo(self) work; new posts after this capture a fresh snap.
  impl_->deferred.Invalidate();
}

CircuitTunnelId CircuitClientCoordinator::StartBridge(const std::string& relay_peer_key,
                                                      const CircuitBridgeTarget& target_in, FrameHandler on_payload,
                                                      ClosedCallback on_closed, BridgeFinished on_finished,
                                                      const int timeout_ms) {
  if (impl_->started.load(std::memory_order_acquire) && target_in.target_multiaddr.empty() &&
      target_in.target_peer_id.empty()) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("missing circuit bridge target"));
      });
    }
    return {};
  }
  if (impl_->RefuseStart(relay_peer_key, on_finished)) {
    return {};
  }
  auto tunnel = std::make_unique<Impl::Tunnel>();
  tunnel->relay_peer_key = relay_peer_key;
  tunnel->target = target_in;
  if (tunnel->target.target_protocol.empty()) {
    tunnel->target.target_protocol = kCircuitRelayProtocolId;
  }
  tunnel->on_payload = std::move(on_payload);
  tunnel->on_closed = std::move(on_closed);
  tunnel->on_finished = std::move(on_finished);
  tunnel->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
  return impl_->Launch(std::move(tunnel));
}

CircuitTunnelId CircuitClientCoordinator::StartReserve(const std::string& relay_peer_key, BridgeFinished on_finished,
                                                       const int timeout_ms) {
  if (impl_->RefuseStart(relay_peer_key, on_finished)) {
    return {};
  }
  auto tunnel = std::make_unique<Impl::Tunnel>();
  tunnel->is_reserve = true;
  tunnel->relay_peer_key = relay_peer_key;
  tunnel->on_finished = std::move(on_finished);
  tunnel->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
  return impl_->Launch(std::move(tunnel));
}

void CircuitClientCoordinator::CancelTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  impl_->PostIo([impl = impl_.get(), id] {
    std::lock_guard lock(impl->mu);
    if (auto* tunnel = impl->Find(id)) {
      impl->TearDown(*tunnel, true, true, "circuit-relay aborted");
    }
  });
}

CircuitTunnelPhase CircuitClientCoordinator::Phase(const CircuitTunnelId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* tunnel = impl_->Find(id)) {
    return tunnel->phase;
  }
  return CircuitTunnelPhase::Idle;
}

bool CircuitClientCoordinator::IsTunnelActive(const CircuitTunnelId id) const {
  return CircuitTunnelPhaseIsActive(Phase(id));
}

size_t CircuitClientCoordinator::ParkedRelayCount() const {
  std::lock_guard lock(impl_->mu);
  return impl_->reserved_relays.size();
}

size_t CircuitClientCoordinator::TunnelCount() const {
  std::lock_guard lock(impl_->mu);
  return impl_->tunnels.size();
}

std::shared_ptr<pp::amp::ChannelSession> CircuitClientCoordinator::Session(const CircuitTunnelId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* tunnel = impl_->Find(id)) {
    return tunnel->near_session;
  }
  return nullptr;
}

} // namespace pbr
