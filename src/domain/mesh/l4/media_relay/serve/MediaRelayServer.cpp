#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"

#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/l4/media_relay/MediaRelayBundleLogic.h"
#include "domain/mesh/l4/media_relay/MediaRelayFrames.h"
#include "domain/mesh/l4/media_relay/MediaRelayLogic.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayAttachSm.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
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

std::string MakeSessionToken() {
  static std::atomic<uint64_t> seq{1};
  std::ostringstream oss;
  oss << "s" << seq.fetch_add(1, std::memory_order_relaxed);
  return oss.str();
}

} // namespace

struct MediaRelayServer::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  mutable std::mutex mu;
  MediaRelayAdmissionPolicy admission;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<bool> serve_inbound{true};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  DeferredSelf deferred;
  /** IoTick / protocol handler — Invalidate only on Stop (survives mid-life Abort). */
  DeferredSelf lifetime;

  struct HostParticipant {
    std::string peer_id;
    std::shared_ptr<pp::amp::ChannelSession> channel;
    /** Set for the local participant (this node's client): frames go here, not to a channel. */
    FrameHandler local_on_frame;
    std::unordered_set<uint64_t> subscriptions;
    std::unordered_map<uint64_t, uint32_t> last_lossy_seq;
  };

  struct HostSession {
    std::string call_id;
    std::string session_token;
    std::vector<std::shared_ptr<HostParticipant>> participants;
  };

  /** Quotes issued and not yet accepted — expire / capped. */
  MediaRelayQuoteBook quotes;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> hosts_by_call;
  std::shared_ptr<HostParticipant> local_part_;
  std::shared_ptr<HostSession> local_session_;
  std::string local_peer_id_;

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); },
                  std::move(task));
  }

  pp::amp::PeerLink* ResolveLink(const std::string& peer_key) const {
    if (!runtime || peer_key.empty()) {
      return nullptr;
    }
    return runtime->Links().FindLink(peer_key);
  }

  void Tick() {
    std::lock_guard lock(mu);
    quotes.Expire(Clock::now());
  }

  static void EraseParticipant(HostSession& session, const HostParticipant* part) {
    session.participants.erase(
        std::remove_if(session.participants.begin(), session.participants.end(),
                       [part](const std::shared_ptr<HostParticipant>& p) { return p.get() == part; }),
        session.participants.end());
  }

  /** Requires `mu`. */
  void DetachLocalLocked() {
    if (local_part_) {
      local_part_->local_on_frame = nullptr;
      if (local_session_) {
        EraseParticipant(*local_session_, local_part_.get());
      }
    }
    local_part_.reset();
    local_session_.reset();
    local_peer_id_.clear();
  }

  /**
   * Fan-out must not run while `mu` is held across callbacks / EnqueueOutbound.
   * Snapshot participants under lock (same pattern as MediaRelayRuntime::Fanout).
   */
  void Fanout(const std::shared_ptr<HostSession>& session, const std::string& from_peer,
              const MediaDataFrame& frame, const std::vector<uint8_t>& body) {
    if (!session) {
      return;
    }
    const uint64_t key = MediaRelaySubKey(frame.stream_id, frame.channel_id);
    std::vector<std::shared_ptr<HostParticipant>> parts;
    {
      std::lock_guard lock(mu);
      parts = session->participants;
    }
    for (const auto& part : parts) {
      if (!part || part->peer_id == from_peer) {
        continue;
      }
      FrameHandler on_frame;
      std::shared_ptr<pp::amp::ChannelSession> channel;
      {
        std::lock_guard lock(mu);
        if (part->subscriptions.find(key) == part->subscriptions.end()) {
          continue;
        }
        if (frame.channel_type == MediaChannelType::LatestLossy) {
          auto it = part->last_lossy_seq.find(key);
          if (ShouldDropStaleLossyFrame(it != part->last_lossy_seq.end(),
                                        it != part->last_lossy_seq.end() ? it->second : 0, frame.seq,
                                        frame.mark)) {
            continue;
          }
          part->last_lossy_seq[key] = frame.seq;
        }
        if (part->local_on_frame) {
          on_frame = part->local_on_frame;
        } else {
          channel = part->channel;
        }
      }
      if (on_frame) {
        on_frame(frame);
        continue;
      }
      if (channel) {
        (void)channel->EnqueueOutbound(body);
      }
    }
  }

  static void SendAck(pp::amp::ChannelSession& channel, const char* op) {
    Object ack;
    ack.set("v", int64_t{1});
    ack.set("ok", true);
    ack.set("op", op);
    channel.EnqueueOutbound(JsonToBody(DumpJson(ack)));
  }

  /** Requires `mu` held. */
  bool HandleParticipantControl(const std::shared_ptr<HostSession>& session,
                                const std::shared_ptr<HostParticipant>& part, const Object& root) {
    const std::string op = root.getString("op").value_or("");
    const uint32_t stream_id = static_cast<uint32_t>(root.getNonNegInt("stream_id").value_or(0));
    const uint16_t channel_id = static_cast<uint16_t>(root.getNonNegInt("channel_id").value_or(0));
    if (op == "subscribe") {
      part->subscriptions.insert(MediaRelaySubKey(stream_id, channel_id));
      if (part->channel) {
        SendAck(*part->channel, "subscribe");
      }
      return true;
    }
    if (op == "unsubscribe") {
      part->subscriptions.erase(MediaRelaySubKey(stream_id, channel_id));
      if (part->channel) {
        SendAck(*part->channel, "unsubscribe");
      }
      return true;
    }
    if (op == "detach") {
      if (part->channel) {
        SendAck(*part->channel, "detach");
        CloseQuietSlot(part->channel, ResolveLink(part->peer_id));
      }
      EraseParticipant(*session, part.get());
      return false;
    }
    return true;
  }

  /** Must not be called with `mu` held (Fanout locks internally). */
  bool HandleParticipantFrame(const std::shared_ptr<HostSession>& session,
                              const std::shared_ptr<HostParticipant>& part, const std::vector<uint8_t>& body) {
    if (body.empty()) {
      return true;
    }
    if (body[0] == '{') {
      auto root = TryParseObject(BodyToJson(body));
      if (!root) {
        return true;
      }
      std::lock_guard lock(mu);
      return HandleParticipantControl(session, part, *root);
    }
    auto frame = DecodeMediaDataFrame(body);
    if (!frame) {
      return true;
    }
    Fanout(session, part->peer_id, *frame, body);
    return true;
  }

  void RebindParticipantHandlers(const std::shared_ptr<HostSession>& session,
                                 const std::shared_ptr<HostParticipant>& part) {
    if (!part || !part->channel) {
      return;
    }
    part->channel->SetFrameHandler([this, session, part](Roe<std::vector<uint8_t>> frame) {
      if (!frame) {
        if (part->channel) {
          CloseQuietSlot(part->channel, ResolveLink(part->peer_id));
        }
        std::lock_guard lock(mu);
        EraseParticipant(*session, part.get());
        return false;
      }
      return HandleParticipantFrame(session, part, *frame);
    });
  }

  /** Sync host cleanup under `mu` (no PostIo with raw this — Stop lifetime). */
  void ClearHostsLocked() {
    for (auto& [_, host] : hosts_by_call) {
      if (!host) {
        continue;
      }
      for (auto& part : host->participants) {
        if (part && part->channel) {
          CloseQuietSlot(part->channel, ResolveLink(part->peer_id));
        }
      }
      host->participants.clear();
    }
    hosts_by_call.clear();
    quotes.Clear();
  }

  void Reject(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const std::string& error,
              const MediaRelayAttachEvent ev) {
    Object err;
    err.set("v", int64_t{1});
    err.set("ok", false);
    err.set("error", error);
    channel.EnqueueOutbound(JsonToBody(DumpJson(err)));
    (void)sm.Apply(ev);
    channel.Close();
  }

  /** Requires `mu`: the participant bound to `channel`, searching every hosted session. */
  bool FindParticipantByChannel(const pp::amp::ChannelSession* channel, std::shared_ptr<HostSession>& host,
                                std::shared_ptr<HostParticipant>& part) {
    for (auto& [call_id_key, host_session] : hosts_by_call) {
      (void)call_id_key;
      for (const auto& candidate : host_session->participants) {
        if (candidate && candidate->channel.get() == channel) {
          host = host_session;
          part = candidate;
          return true;
        }
      }
    }
    return false;
  }

  /** Media frame from an attached participant (not JSON). */
  bool HandleInboundMedia(pp::amp::ChannelSession& channel, const MediaRelayAttachSm& sm,
                          const std::vector<uint8_t>& frame) {
    std::shared_ptr<HostSession> host;
    std::shared_ptr<HostParticipant> part;
    {
      std::lock_guard lock(mu);
      if (sm.phase != MediaRelayAttachPhase::Attached || sm.call_id.empty()) {
        return true;
      }
      auto host_it = hosts_by_call.find(sm.call_id);
      if (host_it == hosts_by_call.end()) {
        return true;
      }
      for (const auto& candidate : host_it->second->participants) {
        if (candidate && candidate->channel.get() == &channel) {
          host = host_it->second;
          part = candidate;
          break;
        }
      }
    }
    if (host && part) {
      return HandleParticipantFrame(host, part, frame);
    }
    return true;
  }

  /** Requires `mu`. */
  bool HandleQuote(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const Object& root,
                   const MediaRelayOpAdmitContext& admit) {
    const auto decision = DecideMediaRelayOpAdmit(admit);
    if (decision != MediaRelayOpAdmitDecision::Allow) {
      Reject(channel, sm,
             decision == MediaRelayOpAdmitDecision::RefuseStranger ? "prefer contacts: stranger refused"
                                                                   : "media-relay not ready",
             MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    MediaRelayQuoteRequest req;
    req.session_id = root.getString("call_id").value_or("");
    req.participants = static_cast<int>(root.getNonNegInt("participants").value_or(1));
    req.want_up_bps = root.getIf<int64_t>("want_up_bps").value_or(0);
    req.want_down_bps = root.getIf<int64_t>("want_down_bps").value_or(0);
    sm.call_id = req.session_id;
    auto q = BuildDefaultMediaRelayQuote(req);
    if (!quotes.Add(q, req.session_id, Clock::now())) {
      Reject(channel, sm, "media-relay busy", MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    Object quote_resp;
    quote_resp.set("v", int64_t{1});
    quote_resp.set("ok", true);
    quote_resp.set("op", "quote");
    quote_resp.set("quote_id", q.quote_id);
    quote_resp.set("A_up", q.a_up_bps);
    quote_resp.set("A_down", q.a_down_bps);
    quote_resp.set("B_up", q.b_up_bps);
    quote_resp.set("B_down", q.b_down_bps);
    quote_resp.set("mode", q.pricing_mode);
    quote_resp.set("rate", q.rate);
    quote_resp.set("ceiling_bytes", q.ceiling_bytes);
    quote_resp.set("ceiling_amount", q.ceiling_amount);
    channel.EnqueueOutbound(JsonToBody(DumpJson(quote_resp)));
    (void)sm.Apply(MediaRelayAttachEvent::OpQuote);
    return true;
  }

  /** Requires `mu`. */
  bool HandleAccept(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const Object& root,
                    MediaRelayOpAdmitContext admit) {
    if (sm.phase != MediaRelayAttachPhase::Control && sm.phase != MediaRelayAttachPhase::Quoted) {
      Reject(channel, sm, "accept not allowed in phase", MediaRelayAttachEvent::OpAccept);
      return false;
    }
    const std::string quote_id = root.getString("quote_id").value_or("");
    auto pending = quotes.Take(quote_id, Clock::now());
    if (!pending) {
      Reject(channel, sm, "unknown quote", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    admit.call_id = pending->call_id;
    admit.session_exists_for_call = hosts_by_call.contains(admit.call_id);
    if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
      Reject(channel, sm, "prefer contacts: stranger refused", MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    sm.call_id = pending->call_id;
    sm.accepted_quote_id = quote_id;
    sm.session_token = MakeSessionToken();
    Object accept_resp;
    accept_resp.set("v", int64_t{1});
    accept_resp.set("ok", true);
    accept_resp.set("op", "accept");
    accept_resp.set("session_token", sm.session_token);
    accept_resp.set("quote_id", sm.accepted_quote_id);
    channel.EnqueueOutbound(JsonToBody(DumpJson(accept_resp)));
    (void)sm.Apply(MediaRelayAttachEvent::OpAccept);
    return true;
  }

  /** Requires `mu`. */
  bool HandleAttach(const std::shared_ptr<pp::amp::ChannelSession>& channel, MediaRelayAttachSm& sm,
                    const Object& root, MediaRelayOpAdmitContext admit) {
    if (!sm.Apply(MediaRelayAttachEvent::OpAttach)) {
      Reject(*channel, sm, "attach not allowed in phase", MediaRelayAttachEvent::OpAttach);
      return false;
    }
    const std::string token = root.getString("session_token").value_or(sm.session_token);
    const std::string call_id = root.getString("call_id").value_or("");
    const std::string auth = root.getString("auth").value_or("");
    sm.call_id = call_id;
    if (token.empty() || call_id.empty()) {
      Reject(*channel, sm, "missing session_token or call_id", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (!MediaRelayAuthStubOk(auth, call_id)) {
      Reject(*channel, sm, "auth failed", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    admit.call_id = call_id;
    admit.session_exists_for_call = hosts_by_call.contains(call_id);
    if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
      Reject(*channel, sm, "prefer contacts: stranger refused", MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    std::shared_ptr<HostSession> host = hosts_by_call[call_id];
    if (!host) {
      host = std::make_shared<HostSession>();
      host->call_id = call_id;
      host->session_token = token;
      hosts_by_call[call_id] = host;
    }
    auto part = std::make_shared<HostParticipant>();
    part->peer_id = sm.remote;
    part->channel = channel;
    host->participants.push_back(part);
    PostIo([this, host, part] { RebindParticipantHandlers(host, part); });
    SendAck(*channel, "attach");
    (void)sm.Apply(MediaRelayAttachEvent::AttachOk);
    return true;
  }

  /** Control JSON on an inbound channel: quote → accept → attach, then participant control. */
  bool HandleInboundControl(const std::shared_ptr<pp::amp::ChannelSession>& channel, MediaRelayAttachSm& sm,
                            const std::vector<uint8_t>& frame) {
    auto root = TryParseObject(BodyToJson(frame));
    if (!root) {
      Reject(*channel, sm, "invalid media-relay json", MediaRelayAttachEvent::Cancel);
      return false;
    }
    const std::string op = root->getString("op").value_or("");
    std::lock_guard lock(mu);
    MediaRelayOpAdmitContext admit;
    admit.service_started = started.load(std::memory_order_acquire) && serve_inbound.load(std::memory_order_acquire);
    admit.stopping = stopped.load(std::memory_order_acquire);
    admit.dialer_peer_id = sm.remote;
    admit.op = op;
    admit.call_id = root->getString("call_id").value_or(sm.call_id);
    admit.session_exists_for_call = !admit.call_id.empty() && hosts_by_call.contains(admit.call_id);
    admit.serve_scope_mask = admission.serve_scope_mask;
    admit.contact_peer_ids = admission.contact_peer_ids;

    if (op == "quote") {
      return HandleQuote(*channel, sm, *root, admit);
    }
    if (op == "accept") {
      return HandleAccept(*channel, sm, *root, std::move(admit));
    }
    if (op == "attach") {
      return HandleAttach(channel, sm, *root, std::move(admit));
    }
    if (sm.phase == MediaRelayAttachPhase::Attached) {
      std::shared_ptr<HostSession> host;
      std::shared_ptr<HostParticipant> part;
      if (FindParticipantByChannel(channel.get(), host, part)) {
        return HandleParticipantControl(host, part, *root);
      }
    }
    Reject(*channel, sm, "unsupported op", MediaRelayAttachEvent::OpUnsupported);
    return false;
  }

  void HandleInboundChannel(const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || remote_peer_id.empty()) {
      return;
    }
    auto channel_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto sm = std::make_shared<MediaRelayAttachSm>();
    sm->remote = remote_peer_id;
    (void)sm->Apply(MediaRelayAttachEvent::StreamOpened);

    *channel_holder = runtime->Links().BindChannel(
        remote_peer_id, channel_id, pp::amp::MediaRelayClientChannelPolicy(),
        [this, channel_holder, sm](Roe<std::vector<uint8_t>> frame) {
          auto channel = *channel_holder;
          if (!channel || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          if (frame->empty()) {
            return true;
          }
          if (frame->front() != '{') {
            return HandleInboundMedia(*channel, *sm, *frame);
          }
          return HandleInboundControl(channel, *sm, *frame);
        },
        // The handler (owned by the channel) captures this holder, which owns the channel: drop the
        // holder's reference when the channel ends, or every served session leaks (LeakSanitizer).
        [channel_holder](const char* /*reason*/) { channel_holder->reset(); });
  }
};

MediaRelayServer::MediaRelayServer(pp::amp::MeshRuntime& runtime)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

MediaRelayServer::~MediaRelayServer() {
  Stop();
}

void MediaRelayServer::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick(impl_->lifetime.Bind([impl = impl_.get()] { impl->Tick(); }));
  runtime_.Links().SetProtocolHandler(
      kMediaRelayProtocolId,
      impl_->lifetime.Bind([impl = impl_.get()](pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id,
                                                const uint32_t ch) { impl->HandleInboundChannel(remote_peer_id, ch); }));
}

void MediaRelayServer::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!impl_->started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  runtime_.Links().RemoveProtocolHandler(kMediaRelayProtocolId);
  AbortInflight();
  impl_->lifetime.Invalidate();
}

bool MediaRelayServer::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void MediaRelayServer::SetServeInbound(const bool serve) {
  impl_->serve_inbound.store(serve, std::memory_order_release);
}

bool MediaRelayServer::ServeInbound() const {
  return impl_->serve_inbound.load(std::memory_order_acquire);
}

void MediaRelayServer::SetAdmissionPolicy(MediaRelayAdmissionPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->admission = std::move(policy);
}

void MediaRelayServer::AbortInflight() {
  // Strand before mu (IO callbacks hold the strand); see CircuitClientCoordinator::AbortInflight.
  runtime_.WithIoLock([&]() {
    std::lock_guard lock(impl_->mu);
    impl_->DetachLocalLocked();
    impl_->ClearHostsLocked();
  });
  // Poison already-queued PostIo(self) work; new posts after this capture a fresh snap.
  impl_->deferred.Invalidate();
}

Roe<MediaRelayAttachResult> MediaRelayServer::AttachLocal(const std::string& call_id,
                                                          const std::string& local_peer_id, FrameHandler on_frame) {
  if (call_id.empty()) {
    return Error("missing call_id");
  }
  if (local_peer_id.empty()) {
    return Error("amp media-relay: missing local peer id");
  }
  std::lock_guard lock(impl_->mu);
  if (impl_->local_part_ && impl_->local_session_ && impl_->local_session_->call_id == call_id &&
      impl_->local_peer_id_ == local_peer_id) {
    impl_->local_part_->local_on_frame = std::move(on_frame);
    MediaRelayAttachResult out;
    out.ok = true;
    out.session_token = impl_->local_session_->session_token;
    return out;
  }
  impl_->DetachLocalLocked();
  auto part = std::make_shared<Impl::HostParticipant>();
  part->peer_id = local_peer_id;
  part->local_on_frame = std::move(on_frame);
  std::shared_ptr<Impl::HostSession> session;
  if (auto it = impl_->hosts_by_call.find(call_id); it != impl_->hosts_by_call.end()) {
    session = it->second;
  } else {
    session = std::make_shared<Impl::HostSession>();
    session->call_id = call_id;
    session->session_token = MakeSessionToken();
    impl_->hosts_by_call[call_id] = session;
  }
  session->participants.erase(std::remove_if(session->participants.begin(), session->participants.end(),
                                             [&](const std::shared_ptr<Impl::HostParticipant>& p) {
                                               return p && p->peer_id == part->peer_id && p->local_on_frame;
                                             }),
                              session->participants.end());
  session->participants.push_back(part);
  impl_->local_part_ = part;
  impl_->local_session_ = session;
  impl_->local_peer_id_ = local_peer_id;
  MediaRelayAttachResult out;
  out.ok = true;
  out.session_token = session->session_token;
  return out;
}

bool MediaRelayServer::LocalAttachedTo(const std::string& call_id, const std::string& local_peer_id) const {
  std::lock_guard lock(impl_->mu);
  return impl_->local_part_ && impl_->local_session_ && impl_->local_session_->call_id == call_id &&
         impl_->local_peer_id_ == local_peer_id;
}

void MediaRelayServer::DetachLocal() {
  std::lock_guard lock(impl_->mu);
  impl_->DetachLocalLocked();
}

bool MediaRelayServer::IsLocalAttached() const {
  std::lock_guard lock(impl_->mu);
  return impl_->local_part_ != nullptr;
}

bool MediaRelayServer::SubscribeLocal(const uint32_t stream_id, const uint16_t channel_id) {
  std::lock_guard lock(impl_->mu);
  if (!impl_->local_part_) {
    return false;
  }
  impl_->local_part_->subscriptions.insert(MediaRelaySubKey(stream_id, channel_id));
  return true;
}

bool MediaRelayServer::SendLocal(const MediaDataFrame& frame) {
  std::shared_ptr<Impl::HostSession> session;
  std::string from_peer;
  {
    std::lock_guard lock(impl_->mu);
    if (!impl_->local_part_) {
      return false;
    }
    session = impl_->local_session_;
    from_peer = impl_->local_peer_id_;
  }
  // The local participant's sender is the capture thread: enqueue onto the other participants'
  // channels under the runtime io lock (io-affine sessions; see CallMediaLegCoordinator::SendMedia).
  // Io lock → `mu` (Fanout locks it) is the io tick's order.
  const std::vector<uint8_t> body = EncodeMediaDataFrame(frame);
  runtime_.WithIoLock([&]() { impl_->Fanout(session, from_peer, frame, body); });
  return true;
}

} // namespace pbr
