#include "domain/mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "domain/mesh/l4/media_relay/MediaRelayVideoLevels.h"

#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/l4/media_relay/MediaRelayFrames.h"
#include "domain/mesh/l4/media_relay/MediaRelayLogic.h"
#include "common/ValueJson.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "foundation/runtime/DeferredSelf.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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

MediaRelayQuote ParseQuoteResponse(const Object& root) {
  MediaRelayQuote q;
  q.ok = root.getIf<bool>("ok").value_or(false);
  q.error = root.getString("error").value_or("");
  q.quote_id = root.getString("quote_id").value_or("");
  q.a_up_bps = root.getIf<int64_t>("A_up").value_or(0);
  q.a_down_bps = root.getIf<int64_t>("A_down").value_or(0);
  q.b_up_bps = root.getIf<int64_t>("B_up").value_or(0);
  q.b_down_bps = root.getIf<int64_t>("B_down").value_or(0);
  q.pricing_mode = root.getString("mode").value_or("volunteer");
  q.rate = root.getIf<double>("rate").value_or(0.0);
  q.ceiling_bytes = root.getIf<int64_t>("ceiling_bytes").value_or(0);
  q.ceiling_amount = root.getIf<double>("ceiling_amount").value_or(0.0);
  q.video_levels = VideoLevelsFromJson(root, "video_levels");
  return q;
}

} // namespace

struct MediaRelayClientCoordinator::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  MediaRelayServer* local_server = nullptr;
  AmpCircuitHopRegistry* circuit_hops = nullptr;
  mutable std::mutex mu;
  std::atomic<bool> started{false};
  std::atomic<uint64_t> next_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  DeferredSelf deferred;
  /** IoTick — Invalidate only on Stop (survives mid-life Abort). */
  DeferredSelf lifetime;

  struct ClientState {
    std::shared_ptr<pp::amp::ChannelSession> channel;
    std::string hop_peer_key;
    std::string call_id;
    FrameHandler on_frame;
    bool reader_started = false;
    std::unordered_set<uint64_t> subscriptions;
  };
  /**
   * Registrations, not session state: they outlive every attach / detach of `client_` (features
   * register once at wiring — dropping them with the session disabled reattach-on-loss after the
   * first attach).
   */
  std::map<uint64_t, std::function<void(MediaRelayClientLoss)>> client_lost_observers_;
  uint64_t next_observer_token_ = 1;

  /** Session-end notices waiting for the next io tick (guarded by `mu`). */
  std::vector<std::function<void()>> pending_client_notices_;

  /**
   * Caller holds `mu`. Only queues: posting to the runtime here would take the runtime lock under
   * `mu` (the io tick takes them in the other order). Delivered by DeliverClientNotices.
   */
  void NotifyClientObserversLocked(MediaRelayClientLoss loss) {
    if (client_lost_observers_.empty()) {
      return;
    }
    std::vector<std::function<void(MediaRelayClientLoss)>> observers;
    for (const auto& [token, observer] : client_lost_observers_) {
      (void)token;
      observers.push_back(observer);
    }
    pending_client_notices_.push_back([observers = std::move(observers), loss]() {
      for (const auto& observer : observers) {
        observer(loss);
      }
    });
  }

  /** Io tick, without `mu`: run queued session-end notices. */
  void DeliverClientNotices() {
    std::vector<std::function<void()>> notices;
    {
      std::lock_guard lock(mu);
      notices.swap(pending_client_notices_);
    }
    for (const auto& notice : notices) {
      notice();
    }
  }

  /** One outbound quote or accept → attach bundle, until it finishes or is adopted as `client_`. */
  struct Session {
    MediaRelaySessionId id;
    MediaRelayBundleRole role = MediaRelayBundleRole::ClientQuote;
    MediaRelayBundlePhase phase = MediaRelayBundlePhase::Idle;
    Clock::time_point deadline{};
    std::string hop_peer_key;
    std::string call_id;
    std::string quote_id;
    std::string auth_stub;
    std::string session_token;
    std::shared_ptr<pp::amp::ChannelSession> channel;
    bool circuit_backed = false;
    QuoteFinished on_quote;
    AttachFinished on_attach;
    FrameHandler on_frame;
    bool finished = false;
    bool local_cancel = false;
  };

  std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions;
  ClientState client_;

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); },
                  std::move(task));
  }

  Session* Find(const MediaRelaySessionId id) {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  const Session* Find(const MediaRelaySessionId id) const {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  void ScheduleWhenChannelOpen(const std::string& peer_key, const uint32_t channel_id,
                               const Clock::time_point deadline, std::function<void(bool open)> done) {
    if (!runtime || peer_key.empty()) {
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

  /** PeerLink drop leaves ChannelSession mux_ dangling — tear down before L4 touches it. */
  bool PeerLinkMissing(const Session& session) const {
    if (!runtime || session.hop_peer_key.empty() || session.circuit_backed) {
      return false;
    }
    return runtime->Links().FindLink(session.hop_peer_key) == nullptr;
  }

  void TickDeadlines() {
    DeliverClientNotices();
    const auto now = Clock::now();
    std::vector<MediaRelaySessionId> timed_out;
    std::vector<MediaRelaySessionId> link_lost;
    {
      std::lock_guard lock(mu);
      for (auto& [_, session] : sessions) {
        if (!session || session->phase == MediaRelayBundlePhase::Closing) {
          continue;
        }
        if (PeerLinkMissing(*session)) {
          link_lost.push_back(session->id);
          continue;
        }
        if (session->finished || session->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= session->deadline && session->phase != MediaRelayBundlePhase::Attached &&
            session->phase != MediaRelayBundlePhase::HostServe) {
          timed_out.push_back(session->id);
        }
      }
    }
    for (const auto id : link_lost) {
      std::lock_guard lock(mu);
      if (auto* session = Find(id)) {
        if (session->phase == MediaRelayBundlePhase::Closing) {
          continue;
        }
        TearDown(*session, false, "media-relay peer link lost");
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* session = Find(id)) {
        TearDown(*session, false, "media-relay timed out");
      }
    }
  }

  void FinishQuote(Session& session, Roe<MediaRelayQuote> result) {
    if (session.finished) {
      return;
    }
    session.finished = true;
    auto cb = std::move(session.on_quote);
    session.on_quote = nullptr;
    if (cb) {
      cb(std::move(result));
    }
  }

  void FinishAttach(Session& session, Roe<MediaRelayAttachResult> result) {
    if (session.finished) {
      return;
    }
    session.finished = true;
    auto cb = std::move(session.on_attach);
    session.on_attach = nullptr;
    if (cb) {
      cb(std::move(result));
    }
  }

  void TearDown(Session& session, const bool local_cancel, const std::string& error) {
    session.local_cancel = local_cancel || session.local_cancel;
    session.phase = MediaRelayBundlePhase::Closing;
    if (session.channel) {
      CloseQuietSlot(session.channel, ResolveLink(session.hop_peer_key));
    }
    if (!session.finished) {
      if (session.role == MediaRelayBundleRole::ClientQuote) {
        FinishQuote(session, Error(error.empty() ? "media-relay aborted" : error));
      } else if (session.role == MediaRelayBundleRole::ClientAttach) {
        FinishAttach(session, Error(error.empty() ? "media-relay aborted" : error));
      } else {
        session.finished = true;
      }
    }
    sessions.erase(session.id.value);
  }

  /** Requires `mu`. Drops the attached session (remote channel or local hop). Lock order: us → server. */
  void DetachClientLocked() {
    if (client_.channel) {
      CloseQuietSlot(client_.channel, ResolveLink(client_.hop_peer_key));
    }
    client_ = {};
    if (local_server) {
      local_server->DetachLocal();
    }
  }

  bool HandleClientMediaFrame(const std::vector<uint8_t>& body) {
    if (body.empty()) {
      return true;
    }
    if (body[0] == '{') {
      return true; // subscribe ack / control
    }
    auto frame = DecodeMediaDataFrame(body);
    if (!frame) {
      return true;
    }
    FrameHandler on_frame;
    {
      std::lock_guard lock(mu);
      if (!client_.reader_started || !client_.on_frame) {
        return true;
      }
      const uint64_t key = MediaRelaySubKey(frame->stream_id, frame->channel_id);
      if (client_.subscriptions.find(key) == client_.subscriptions.end()) {
        return true;
      }
      on_frame = client_.on_frame;
    }
    if (on_frame) {
      on_frame(*frame);
    }
    return true;
  }

  void RebindClientMediaHandlers() {
    if (!client_.channel) {
      return;
    }
    client_.channel->SetFrameHandler([this](Roe<std::vector<uint8_t>> frame) {
      if (!frame) {
        HandleClientTransportLost("channel failed");
        return false;
      }
      return HandleClientMediaFrame(*frame);
    });
    client_.channel->SetClosedCallback([this](const char* reason) { HandleClientTransportLost(reason); });
  }

  void HandleClientTransportLost(const char* reason) {
    std::lock_guard lock(mu);
    if (!client_.channel) {
      return;
    }
    CloseQuietSlot(client_.channel, ResolveLink(client_.hop_peer_key));
    client_.subscriptions.clear();
    client_.reader_started = false;
    NotifyClientObserversLocked(MediaRelayClientLoss::TransportLost);
    (void)reason;
  }

  /** Move channel to client_ then erase Session — never touch `session` after erase ([A027]). */
  void AdoptClientChannel(Session& session) {
    auto channel = std::move(session.channel);
    const std::string hop = session.hop_peer_key;
    const std::string call_id = session.call_id;
    FrameHandler on_frame = std::move(session.on_frame);
    const uint64_t id = session.id.value;
    const bool circuit_backed = session.circuit_backed;
    sessions.erase(id);

    const bool replaced = static_cast<bool>(client_.channel);
    DetachClientLocked();
    if (replaced) {
      NotifyClientObserversLocked(MediaRelayClientLoss::Replaced);
    }
    // A client media session keeps its hop link hot (K008 "relay outer links"): one-way media
    // (a publisher only sends, a viewer only receives) gives one end no RX, and a cold link is
    // evicted after 5 s of silence. Hot keepalives carry an echo. Never cleared on detach — the
    // same node is often our circuit relay, whose reservation needs the hot tier too. Marked on the
    // io tick, without `mu` (the link strand calls into us holding its own lock).
    if (!circuit_backed && runtime) {
      pending_client_notices_.push_back([rt = runtime, hop]() { rt->Links().MarkHot(hop); });
    }
    client_.channel = std::move(channel);
    client_.hop_peer_key = hop;
    client_.call_id = call_id;
    client_.on_frame = std::move(on_frame);
    client_.reader_started = false;
    client_.subscriptions.clear();
    RebindClientMediaHandlers();
  }

  /** Requires `mu`. Quote reply, or accept → attach replies, on a bundle's channel. */
  bool HandleSessionFrame(const MediaRelaySessionId id, Roe<std::vector<uint8_t>> frame) {
    auto* session = Find(id);
    if (!session) {
      return false;
    }
    if (!frame) {
      TearDown(*session, false, "media-relay channel failed");
      return false;
    }
    auto root = TryParseObject(BodyToJson(*frame));
    if (!root) {
      TearDown(*session, false, "invalid media-relay json");
      return false;
    }
    if (session->role == MediaRelayBundleRole::ClientQuote && session->phase == MediaRelayBundlePhase::WaitQuote) {
      const auto decision =
          DecideMediaRelayQuoteAck({.phase = session->phase, .ack_ok = root->getIf<bool>("ok").value_or(false)});
      if (decision == MediaRelayQuoteAckDecision::IgnoreStale) {
        return true;
      }
      if (decision == MediaRelayQuoteAckDecision::Fail) {
        TearDown(*session, false, root->getString("error").value_or("quote failed"));
        return false;
      }
      auto quote = ParseQuoteResponse(*root);
      session->phase = MediaRelayBundlePhase::Closing;
      FinishQuote(*session, std::move(quote));
      // Direct quote channels are one-shot; circuit hops must stay open for attach.
      if (session->channel && !session->circuit_backed) {
        CloseQuietSlot(session->channel, ResolveLink(session->hop_peer_key));
        sessions.erase(id.value);
        return false;
      }
      sessions.erase(id.value);
      return true;
    }
    if (session->role != MediaRelayBundleRole::ClientAttach) {
      return true;
    }
    if (session->phase == MediaRelayBundlePhase::WaitAccept) {
      if (!root->getIf<bool>("ok").value_or(false)) {
        TearDown(*session, false, root->getString("error").value_or("accept failed"));
        return false;
      }
      session->session_token = root->getString("session_token").value_or("");
      session->phase = MediaRelayBundlePhase::OutboundAttach;
      Object attach_req;
      attach_req.set("v", int64_t{1});
      attach_req.set("op", "attach");
      attach_req.set("session_token", session->session_token);
      attach_req.set("call_id", session->call_id);
      attach_req.set("auth", session->auth_stub);
      if (!session->channel->EnqueueOutbound(JsonToBody(DumpJson(attach_req)))) {
        TearDown(*session, false, "failed to send attach");
        return false;
      }
      session->phase = MediaRelayBundlePhase::WaitAttachAck;
      return true;
    }
    if (session->phase == MediaRelayBundlePhase::WaitAttachAck) {
      const auto decision =
          DecideMediaRelayAttachAck({.phase = session->phase, .ack_ok = root->getIf<bool>("ok").value_or(false)});
      if (decision == MediaRelayAttachAckDecision::IgnoreStale) {
        return true;
      }
      if (decision == MediaRelayAttachAckDecision::Fail) {
        TearDown(*session, false, root->getString("error").value_or("attach failed"));
        return false;
      }
      session->phase = MediaRelayBundlePhase::Attached;
      MediaRelayAttachResult out;
      out.ok = true;
      out.session_token = session->session_token;
      FinishAttach(*session, std::move(out));
      AdoptClientChannel(*session);
      return true;
    }
    return true;
  }

  bool TryBeginOnCircuitHop(Session& session, const std::string& outbound_json,
                            const MediaRelayBundlePhase wait_phase) {
    if (!circuit_hops) {
      return false;
    }
    auto hop = circuit_hops->Find(session.hop_peer_key, kMediaRelayProtocolId);
    if (!hop || !hop->session) {
      return false;
    }
    session.channel = hop->session;
    session.circuit_backed = true;
    const MediaRelaySessionId id = session.id;
    session.channel->SetFrameHandler([this, id](Roe<std::vector<uint8_t>> frame) {
      std::lock_guard lock(mu);
      return HandleSessionFrame(id, std::move(frame));
    });
    session.phase = wait_phase;
    if (!session.channel->EnqueueOutbound(JsonToBody(outbound_json))) {
      TearDown(session, false, "failed to send on circuit hop");
    }
    return true;
  }

  void BindClientChannel(Session& session, const std::string& peer_key, const uint32_t channel_id) {
    const MediaRelaySessionId id = session.id;
    session.channel = runtime->Links().BindChannel(
        peer_key, channel_id, pp::amp::MediaRelayClientChannelPolicy(),
        [this, id](Roe<std::vector<uint8_t>> frame) {
          std::lock_guard lock(mu);
          return HandleSessionFrame(id, std::move(frame));
        },
        [this, id](const char*) {
          std::lock_guard lock(mu);
          if (auto* session = Find(id)) {
            const auto decision = DecideMediaRelayBundleClose({
                .phase = session->phase,
                .local_cancel = session->local_cancel,
                .remote_terminal = true,
                .finished = session->finished,
            });
            if (decision != MediaRelayBundleCloseDecision::Ignore) {
              TearDown(*session, session->local_cancel, "media-relay channel closed");
            }
          }
        });
  }

  /** Open a direct channel to the hop, then send `json` and wait in `wait_phase`. */
  void OpenAndSend(Session& session, const std::string& json, const MediaRelayBundlePhase wait_phase,
                   const char* send_error) {
    const MediaRelaySessionId id = session.id;
    const std::string hop = session.hop_peer_key;
    const auto deadline = session.deadline;
    runtime->Links().OpenChannel(
        hop, kMediaRelayProtocolId, pp::amp::MediaRelayClientChannelPolicy(),
        [this, id, hop, deadline, json, wait_phase, send_error](pp::amp::PeerLinkManager::ChannelRoe channel) {
          uint32_t channel_id = 0;
          {
            std::lock_guard lock(mu);
            auto* session = Find(id);
            if (!session) {
              return;
            }
            if (!channel) {
              TearDown(*session, false, channel.error().message);
              return;
            }
            channel_id = *channel;
          }
          ScheduleWhenChannelOpen(
              hop, channel_id, deadline, [this, id, hop, channel_id, json, wait_phase, send_error](const bool open) {
                std::lock_guard lock(mu);
                auto* session = Find(id);
                if (!session) {
                  return;
                }
                if (!open) {
                  TearDown(*session, false, "media-relay: channel open failed");
                  return;
                }
                BindClientChannel(*session, hop, channel_id);
                session->phase = wait_phase;
                if (!session->channel || !session->channel->EnqueueOutbound(JsonToBody(json))) {
                  TearDown(*session, false, send_error);
                }
              });
        });
  }

  void BeginQuote(Session& session, const MediaRelayQuoteRequest& request) {
    session.phase = MediaRelayBundlePhase::OutboundQuote;
    Object req;
    req.set("v", int64_t{1});
    req.set("op", "quote");
    req.set("call_id", request.session_id);
    req.set("participants", int64_t{request.participants});
    req.set("want_up_bps", request.want_up_bps);
    req.set("want_down_bps", request.want_down_bps);
    req.set("video_levels", VideoLevelsToJson(request.video_levels));
    req.set("video_parallel", int64_t{request.video_parallel});
    const std::string json = DumpJson(req);
    if (TryBeginOnCircuitHop(session, json, MediaRelayBundlePhase::WaitQuote)) {
      return;
    }
    OpenAndSend(session, json, MediaRelayBundlePhase::WaitQuote, "failed to send quote");
  }

  void BeginAttach(Session& session) {
    session.phase = MediaRelayBundlePhase::OutboundAccept;
    Object accept_req;
    accept_req.set("v", int64_t{1});
    accept_req.set("op", "accept");
    accept_req.set("quote_id", session.quote_id);
    const std::string json = DumpJson(accept_req);
    if (TryBeginOnCircuitHop(session, json, MediaRelayBundlePhase::WaitAccept)) {
      return;
    }
    OpenAndSend(session, json, MediaRelayBundlePhase::WaitAccept, "failed to send accept");
  }

  /** Posts `on_finished(Error(why))` to io and returns true when a bundle cannot start. */
  template <typename Finished>
  bool RefuseStart(const std::string& hop_peer_key, Finished& on_finished) {
    const char* why = nullptr;
    if (!started.load(std::memory_order_acquire)) {
      why = "media-relay service not started";
    } else if (!runtime->Links().GetLinkSnapshot(hop_peer_key).has_endpoint &&
               !(circuit_hops && circuit_hops->Find(hop_peer_key, kMediaRelayProtocolId))) {
      why = "hop peer endpoint not registered";
    }
    if (!why) {
      return false;
    }
    if (on_finished) {
      runtime->PostToIo([on_finished = std::move(on_finished), why]() mutable { on_finished(Error(why)); });
    }
    return true;
  }
};

MediaRelayClientCoordinator::MediaRelayClientCoordinator(pp::amp::MeshRuntime& runtime,
                                                         MediaRelayServer* local_server)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->local_server = local_server;
}

MediaRelayClientCoordinator::~MediaRelayClientCoordinator() {
  Stop();
}

void MediaRelayClientCoordinator::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->io_tick_id = runtime_.AddIoTick(impl_->lifetime.Bind([impl = impl_.get()] { impl->TickDeadlines(); }));
}

void MediaRelayClientCoordinator::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!impl_->started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  AbortInflight();
  impl_->lifetime.Invalidate();
}

bool MediaRelayClientCoordinator::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void MediaRelayClientCoordinator::SetCircuitHopRegistry(AmpCircuitHopRegistry* hops) {
  std::lock_guard lock(impl_->mu);
  impl_->circuit_hops = hops;
}

void MediaRelayClientCoordinator::AbortInflight() {
  // Sync under lock — never PostIo(raw Impl*) that can outlive Stop/TearDown.
  std::vector<QuoteFinished> quote_cbs;
  std::vector<AttachFinished> attach_cbs;
  // Strand before mu (IO callbacks hold the strand); see CircuitClientCoordinator::AbortInflight.
  runtime_.WithIoLock([&]() {
    std::lock_guard lock(impl_->mu);
    impl_->DetachClientLocked();
    std::vector<uint64_t> ids;
    ids.reserve(impl_->sessions.size());
    for (auto& [id, _] : impl_->sessions) {
      ids.push_back(id);
    }
    for (const auto id : ids) {
      auto* session = impl_->Find(MediaRelaySessionId{id});
      if (!session) {
        continue;
      }
      session->local_cancel = true;
      session->phase = MediaRelayBundlePhase::Closing;
      if (session->channel) {
        CloseQuietSlot(session->channel, impl_->ResolveLink(session->hop_peer_key));
      }
      if (!session->finished) {
        session->finished = true;
        if (session->role == MediaRelayBundleRole::ClientQuote && session->on_quote) {
          quote_cbs.push_back(std::move(session->on_quote));
        } else if (session->role == MediaRelayBundleRole::ClientAttach && session->on_attach) {
          attach_cbs.push_back(std::move(session->on_attach));
        }
      }
      impl_->sessions.erase(id);
    }
  });
  for (auto& cb : quote_cbs) {
    cb(Error("media-relay aborted"));
  }
  for (auto& cb : attach_cbs) {
    cb(Error("media-relay aborted"));
  }
  // Poison already-queued PostIo(self) work; new posts after this capture a fresh snap.
  impl_->deferred.Invalidate();
}

MediaRelaySessionId MediaRelayClientCoordinator::StartQuote(const std::string& hop_peer_key,
                                                            const MediaRelayQuoteRequest& request,
                                                            QuoteFinished on_finished, const int timeout_ms) {
  if (impl_->RefuseStart(hop_peer_key, on_finished)) {
    return {};
  }
  const MediaRelaySessionId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, hop_peer_key, request, on_finished = std::move(on_finished),
                 timeout_ms]() mutable {
    Impl::Session* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto session = std::make_unique<Impl::Session>();
      session->id = id;
      session->role = MediaRelayBundleRole::ClientQuote;
      session->hop_peer_key = hop_peer_key;
      session->call_id = request.session_id;
      session->on_quote = std::move(on_finished);
      session->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      raw = session.get();
      impl->sessions[id.value] = std::move(session);
    }
    impl->BeginQuote(*raw, request);
  });
  return id;
}

MediaRelaySessionId MediaRelayClientCoordinator::StartAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, FrameHandler on_frame, AttachFinished on_finished, const int timeout_ms) {
  if (impl_->started.load(std::memory_order_acquire) && (quote_id.empty() || call_id.empty())) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("missing quote_id or call_id"));
      });
    }
    return {};
  }
  if (impl_->RefuseStart(hop_peer_key, on_finished)) {
    return {};
  }
  const MediaRelaySessionId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, hop_peer_key, quote_id, call_id, auth_stub,
                 on_frame = std::move(on_frame), on_finished = std::move(on_finished),
                 timeout_ms]() mutable {
    Impl::Session* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto session = std::make_unique<Impl::Session>();
      session->id = id;
      session->role = MediaRelayBundleRole::ClientAttach;
      session->hop_peer_key = hop_peer_key;
      session->quote_id = quote_id;
      session->call_id = call_id;
      session->auth_stub = auth_stub;
      session->on_frame = std::move(on_frame);
      session->on_attach = std::move(on_finished);
      session->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      raw = session.get();
      impl->sessions[id.value] = std::move(session);
    }
    impl->BeginAttach(*raw);
  });
  return id;
}

void MediaRelayClientCoordinator::Cancel(const MediaRelaySessionId id) {
  if (!id) {
    return;
  }
  impl_->PostIo([impl = impl_.get(), id] {
    std::lock_guard lock(impl->mu);
    if (auto* session = impl->Find(id)) {
      impl->TearDown(*session, true, "media-relay aborted");
    }
  });
}

MediaRelayBundlePhase MediaRelayClientCoordinator::Phase(const MediaRelaySessionId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* session = impl_->Find(id)) {
    return session->phase;
  }
  return MediaRelayBundlePhase::Idle;
}

bool MediaRelayClientCoordinator::IsSessionActive(const MediaRelaySessionId id) const {
  return MediaRelayBundlePhaseIsActive(Phase(id));
}

void MediaRelayClientCoordinator::StartClientFrameReader() {
  std::lock_guard lock(impl_->mu);
  impl_->client_.reader_started = true;
}

uint64_t MediaRelayClientCoordinator::AddClientTransportLostObserver(
    std::function<void(MediaRelayClientLoss)> observer) {
  std::lock_guard lock(impl_->mu);
  const uint64_t token = impl_->next_observer_token_++;
  impl_->client_lost_observers_.emplace(token, std::move(observer));
  return token;
}

void MediaRelayClientCoordinator::RemoveClientTransportLostObserver(uint64_t token) {
  std::lock_guard lock(impl_->mu);
  impl_->client_lost_observers_.erase(token);
}

Roe<MediaRelayAttachResult> MediaRelayClientCoordinator::AttachAsLocalHop(
    const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) {
  if (call_id.empty()) {
    return Error("missing call_id");
  }
  if (!IsStarted() || !impl_->local_server || !impl_->local_server->IsStarted()) {
    return Error("media_relay not started");
  }
  const std::string local_peer = runtime_.Links().LocalCapability().local_peer_id;
  if (local_peer.empty()) {
    return Error("amp media-relay: missing local peer id");
  }
  std::lock_guard lock(impl_->mu);
  if (!impl_->local_server->LocalAttachedTo(call_id, local_peer)) {
    impl_->DetachClientLocked();
  }
  return impl_->local_server->AttachLocal(call_id, local_peer, std::move(on_frame));
}

Roe<void> MediaRelayClientCoordinator::Subscribe(const uint32_t stream_id, const uint16_t channel_id) {
  const uint64_t key = MediaRelaySubKey(stream_id, channel_id);
  std::shared_ptr<pp::amp::ChannelSession> channel;
  {
    std::lock_guard lock(impl_->mu);
    if (impl_->local_server && impl_->local_server->SubscribeLocal(stream_id, channel_id)) {
      return {};
    }
    if (impl_->client_.subscriptions.count(key) != 0) {
      return {};
    }
    if (!impl_->client_.channel) {
      return Error("not attached");
    }
    impl_->client_.subscriptions.insert(key);
    channel = impl_->client_.channel;
  }
  Object sub;
  sub.set("v", int64_t{1});
  sub.set("op", "subscribe");
  sub.setJsonUInt("stream_id", stream_id);
  sub.setJsonUInt("channel_id", channel_id);
  // Written outside `mu`, under the io lock (see SendFrame).
  const bool sent = runtime_.WithIoLock([&]() { return channel->EnqueueOutbound(JsonToBody(DumpJson(sub))); });
  if (!sent) {
    std::lock_guard lock(impl_->mu);
    impl_->client_.subscriptions.erase(key);
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayClientCoordinator::SendFrame(const MediaDataFrame& frame) {
  std::shared_ptr<pp::amp::ChannelSession> channel;
  {
    std::lock_guard lock(impl_->mu);
    channel = impl_->client_.channel;
  }
  if (!channel) {
    // Local hop: the server fans out from our participant (outside `mu`; Fanout locks internally).
    if (impl_->local_server && impl_->local_server->SendLocal(frame)) {
      return {};
    }
    return Error("not attached");
  }
  // The channel session is io-affine: senders (the engine's capture thread) enqueue under the
  // runtime io lock, or they race the mesh pump on the same mux (failed writes, SIGSEGV). And not
  // under `mu`: a failed write fails the channel synchronously, and its closed callback
  // (HandleClientTransportLost) takes `mu` — enqueueing under it deadlocked the sending thread
  // and then the mesh pump. Order: io lock → mu, as the io tick.
  const std::vector<uint8_t> body = EncodeMediaDataFrame(frame);
  if (!runtime_.WithIoLock([&]() { return channel->EnqueueOutbound(body); })) {
    return Error("not attached");
  }
  return {};
}

void MediaRelayClientCoordinator::Detach() {
  // Client state is cleared synchronously under `mu` (no deferred raw-this PostIo), but the channel
  // closes after `mu` is released: closing takes the mesh runtime lock, and the io tick
  // (TickDeadlines) holds that lock while it takes `mu` — closing under `mu` was a lock-order
  // inversion (UI Detach racing a pump tick could deadlock; TSan).
  std::shared_ptr<pp::amp::ChannelSession> closing;
  std::string hop;
  {
    std::lock_guard lock(impl_->mu);
    closing = std::move(impl_->client_.channel);
    hop = impl_->client_.hop_peer_key;
    impl_->DetachClientLocked();
    if (closing) {
      impl_->NotifyClientObserversLocked(MediaRelayClientLoss::Detached);
    }
  }
  if (closing) {
    runtime_.WithIoLock([&]() { CloseQuietSlot(closing, impl_->ResolveLink(hop)); });
  }
}

bool MediaRelayClientCoordinator::IsAttached() const {
  std::lock_guard lock(impl_->mu);
  return impl_->client_.channel != nullptr || (impl_->local_server && impl_->local_server->IsLocalAttached());
}

bool MediaRelayClientCoordinator::IsLocalHopAttached() const {
  std::lock_guard lock(impl_->mu);
  return impl_->local_server && impl_->local_server->IsLocalAttached();
}

double MediaRelayClientCoordinator::PathPressure() const { return HealthSnapshot().path_pressure; }

CallHopHealth MediaRelayClientCoordinator::HealthSnapshot() const {
  CallHopHealth health;
  health.attached = IsAttached();
  return health;
}

} // namespace pbr
