#include "mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "mesh/l4/media_relay/MediaRelayVideoLevels.h"

#include "mesh/l4/shared/ChannelSessionSlot.h"
#include "mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "mesh/l4/media_relay/MediaRelayFrames.h"
#include "common/ValueJson.h"
#include "mesh/shared/AmpChannelOpen.h"
#include "common/Lifetime.h"
#include "common/Logger.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace pp::mesh {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

MediaRelayQuote ParseQuoteResponse(const common::Object& root) {
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

using Err = MediaRelayErr;
using Failure = MediaRelayFailure;

Failure Aborted() {
  return Failure::Of(Err::Aborted, "media-relay aborted");
}

/** A hop's refusal from its reply (its code; older hops' text). */
Failure Refusal(const common::Object& root, const char* fallback) {
  const std::string text = root.getString("error").value_or("");
  return Failure::Of(MediaRelayErrFromReply(root.getString("code").value_or(""), text),
                     detail::AppendFrom("media-relay: refused", "hop", text.empty() ? std::string(fallback) : text));
}

} // namespace

MediaRelayClientCoordinator::Failure MediaRelayClientCoordinator::WrapLinkFailure(
    const pp::amp::PeerLinkManager::Failure& link) {
  using LinkErr = pp::amp::PeerLinkManager::Err;
  const Err code = link.GetCode() == LinkErr::EndpointNotRegistered ? Err::HopNoEndpoint
                   : link.GetCode() == LinkErr::ChannelOpenFailed   ? Err::ChannelFailed
                                                                    : Err::LinkFailed;
  return Failure::Of(code, detail::AppendFrom("media-relay: hop link failed", "link", link.message));
}

namespace {

auto LaneLog() {
  return logging::getLogger("MediaRelayClient");
}

} // namespace

struct MediaRelayClientCoordinator::Impl {
  /** The hops' levels as they say them (MeshHost's); null: not kept. */
  std::atomic<PeerLoadBook*> loads{nullptr};
  pp::amp::MeshRuntime* runtime = nullptr;
  MediaRelayServer* local_server = nullptr;
  mutable std::mutex mu;
  std::atomic<bool> started{false};
  std::atomic<uint64_t> next_id{1};
  /** Media frames dropped while the hop's data lane was not open (opening, or reopening after it ended). */
  std::atomic<uint64_t> lane_wait_drops{0};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  pp::Lifetime deferred;
  /** IoTick — Invalidate only on Stop (survives mid-life Abort). */
  pp::Lifetime lifetime;

  struct ClientState {
    std::shared_ptr<pp::amp::ChannelSession> channel;
    /**
     * The data lane to the hop (kMediaRelayDataProtocolId) once it opened: media goes and comes there, and only
     * there. The control channel (`channel`) never carries media: while the lane is not open, media is dropped.
     */
    std::shared_ptr<pp::amp::ChannelSession> data_channel;
    /**
     * The attach is reported once its lane opened (`attach_done`, with `attach_result`), or failed: a hop that does
     * not serve the lane, or a lane not open by `attach_deadline`, fails it — no media path, not a usable hop.
     */
    AttachFinished attach_done;
    MediaRelayAttachResult attach_result;
    Clock::time_point attach_deadline{};
    /**
     * The lane is tried again while this attach lasts: an open that timed out (a busy hop, a lossy link), or a lane
     * that ended. `lane_retry_at` empty: no retry pending.
     */
    bool lane_opening = false;
    int lane_failures = 0;
    Clock::time_point lane_retry_at{};
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
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); }, std::move(task));
  }

  Session* Find(const MediaRelaySessionId id) {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  const Session* Find(const MediaRelaySessionId id) const {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  void ScheduleWhenChannelOpen(const std::string& peer_key, const uint32_t channel_id, const Clock::time_point deadline,
                               std::function<void(bool open)> done) {
    if (!runtime || peer_key.empty()) {
      done(false);
      return;
    }
    AmpWhenChannelOpen(runtime->Links(), peer_key, channel_id, deadline, std::move(done));
  }

  /** The hop's link our channels ride: the Connected one OpenChannel took (direct, or nested through a circuit). */
  pp::amp::PeerLink* ResolveLink(const std::string& peer_key) const {
    if (!runtime || peer_key.empty()) {
      return nullptr;
    }
    if (pp::amp::PeerLink* live = runtime->Links().ResolveConnectedLink(peer_key)) {
      return live;
    }
    return runtime->Links().FindLink(peer_key);
  }

  /** PeerLink drop leaves ChannelSession mux_ dangling — tear down before L4 touches it. */
  bool PeerLinkMissing(const Session& session) const {
    if (!runtime || session.hop_peer_key.empty()) {
      return false;
    }
    return ResolveLink(session.hop_peer_key) == nullptr;
  }

  void TickDeadlines() {
    DeliverClientNotices();
    const auto now = Clock::now();
    FailAttachPastDeadline(now);
    RetryDataLaneIfDue(now);
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
        TearDown(*session, false, Failure::Of(Err::Closed, "media-relay peer link lost"));
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* session = Find(id)) {
        TearDown(*session, false, Failure::Of(Err::Timeout, "media-relay timed out"));
      }
    }
  }

  void FinishQuote(Session& session, MediaRelayRoe<MediaRelayQuote> result) {
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

  void FinishAttach(Session& session, MediaRelayRoe<MediaRelayAttachResult> result) {
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

  void TearDown(Session& session, const bool local_cancel, Failure failure) {
    session.local_cancel = local_cancel || session.local_cancel;
    session.phase = MediaRelayBundlePhase::Closing;
    if (session.channel) {
      CloseQuietSlot(session.channel, ResolveLink(session.hop_peer_key));
    }
    if (!session.finished) {
      if (failure.message.empty()) {
        failure = Aborted();
      }
      if (session.role == MediaRelayBundleRole::ClientQuote) {
        FinishQuote(session, failure);
      } else if (session.role == MediaRelayBundleRole::ClientAttach) {
        FinishAttach(session, failure);
      } else {
        session.finished = true;
      }
    }
    sessions.erase(session.id.value);
  }

  /**
   * Requires `mu`. Drops the attached session (remote channel or local hop). Lock order: us → server. A channel
   * still set here is closed here, which resolves the hop's link under the io lock: a caller off the io thread holds
   * that lock first (io → mu), or takes the channels out and closes them after `mu` (Detach).
   */
  void DetachClientLocked() {
    if (client_.attach_done) {
      FinishPendingAttachLocked(Aborted());
    }
    if (client_.data_channel) {
      CloseQuietSlot(client_.data_channel, ResolveLink(client_.hop_peer_key));
    }
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
    if (client_.attach_done) {
      // Lost before its lane opened: the attach fails, nothing was reported attached.
      FinishPendingAttachLocked(Failure::Of(Err::Closed, "media-relay channel closed before its data lane opened"));
      DetachClientLocked();
      return;
    }
    if (client_.data_channel) {
      CloseQuietSlot(client_.data_channel, ResolveLink(client_.hop_peer_key));
      client_.data_channel.reset();
    }
    CloseQuietSlot(client_.channel, ResolveLink(client_.hop_peer_key));
    client_.subscriptions.clear();
    client_.reader_started = false;
    NotifyClientObserversLocked(MediaRelayClientLoss::TransportLost);
    (void)reason;
  }

  /**
   * Requires `mu`. Reports the attach waiting for its lane (`client_.attach_done`), once: ok when the lane opened,
   * else why not. Called under `mu`, as a bundle's own FinishAttach.
   */
  void FinishPendingAttachLocked(MediaRelayRoe<MediaRelayAttachResult> result) {
    AttachFinished done = std::move(client_.attach_done);
    client_.attach_done = nullptr;
    client_.attach_deadline = {};
    if (done) {
      done(std::move(result));
    }
  }

  /** Io tick: an attach whose lane did not open in its time fails (its control channel closes). */
  void FailAttachPastDeadline(const Clock::time_point now) {
    std::lock_guard lock(mu);
    if (!client_.attach_done || client_.attach_deadline == Clock::time_point{} || now < client_.attach_deadline) {
      return;
    }
    LaneLog().info << "media_relay data lane: not open in the attach's time: attach failed call_id=" << client_.call_id;
    FinishPendingAttachLocked(Failure::Of(Err::Timeout, "media-relay: data lane did not open"));
    DetachClientLocked();
  }

  /**
   * Move channel to client_ then erase Session — never touch `session` after erase ([A027]). The attach is
   * reported once the data lane opens (`done`, FinishPendingAttachLocked).
   */
  void AdoptClientChannel(Session& session, AttachFinished done, MediaRelayAttachResult result) {
    auto channel = std::move(session.channel);
    const std::string hop = session.hop_peer_key;
    const std::string call_id = session.call_id;
    FrameHandler on_frame = std::move(session.on_frame);
    const uint64_t id = session.id.value;
    const auto deadline = session.deadline;
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
    if (runtime) {
      pending_client_notices_.push_back([rt = runtime, hop]() { rt->Links().MarkHot(hop); });
    }
    client_.channel = std::move(channel);
    client_.hop_peer_key = hop;
    client_.call_id = call_id;
    client_.on_frame = std::move(on_frame);
    client_.reader_started = false;
    client_.subscriptions.clear();
    client_.attach_done = std::move(done);
    client_.attach_result = std::move(result);
    client_.attach_deadline = deadline;
    RebindClientMediaHandlers();
    // Opened on the io tick, without `mu` (as MarkHot above).
    pending_client_notices_.push_back([this, hop, call_id]() { OpenDataLane(hop, call_id); });
  }

  /**
   * The data lane to `hop` for the attach to `call_id`: its media, best effort, on the same link as its control
   * channel (direct, or nested through a circuit). Not open yet, or ended: media is dropped and the lane tried
   * again. A hop that does not serve it is no media hop: the attach fails (or, attached, the session is lost).
   */
  void OpenDataLane(const std::string& hop, const std::string& call_id) {
    if (!runtime) {
      return;
    }
    const auto still_ours = [this, hop, call_id]() {
      return client_.channel && client_.hop_peer_key == hop && client_.call_id == call_id && !client_.data_channel;
    };
    {
      std::lock_guard lock(mu);
      if (!still_ours()) {
        LaneLog().info << "media_relay data lane: not opened (the attach moved on) call_id=" << call_id;
        return;
      }
      client_.lane_opening = true;
      client_.lane_retry_at = {};
    }
    runtime->Links().OpenChannel(
        hop, kMediaRelayDataProtocolId, pp::amp::MediaRelayDataChannelPolicy(),
        [this, hop, still_ours](pp::amp::PeerLinkManager::ChannelRoe opened) {
          if (!opened) {
            std::lock_guard lock(mu);
            LaneFailedLocked(still_ours(), "open failed", opened.error().message, /*hop_serves_none=*/false);
            return;
          }
          const uint32_t channel_id = *opened;
          // The hop's answer, told to the channel before the waiter below sees it closed: a hop that does not serve
          // the lane says so; anything else — busy, a reset, no answer in time — may pass.
          auto serves_none = std::make_shared<bool>(false);
          if (pp::amp::PeerLink* link = ResolveLink(hop); link && link->Mux()) {
            link->Mux()->SetTerminalHandler(channel_id, [serves_none](uint32_t, const char* reason) {
              *serves_none = reason && std::strcmp(reason, pp::amp::ChannelMux::kOpenRejectedNoHandler) == 0;
            });
          }
          ScheduleWhenChannelOpen(hop, channel_id, Clock::now() + kDataLaneOpenTimeout,
                                  [this, hop, channel_id, still_ours, serves_none](const bool open) {
                                    OnLaneOpenSettled(hop, channel_id, open, !open && *serves_none, still_ours);
                                  });
        });
  }

  /** On the io thread: the lane open ended — open, refused (`refused`: the hop serves no lane), or not open in time. */
  template <typename StillOurs>
  void OnLaneOpenSettled(const std::string& hop, const uint32_t channel_id, const bool open, const bool refused,
                         const StillOurs& still_ours) {
    bool unused = false;
    {
      std::lock_guard lock(mu);
      const bool ours = still_ours();
      if (open && ours) {
        client_.lane_opening = false;
        client_.lane_failures = 0;
        BindDataLaneLocked(hop, channel_id);
        LaneLog().info << "media_relay data lane: open";
        if (client_.attach_done) {
          FinishPendingAttachLocked(client_.attach_result);
        }
      } else {
        unused = !refused; // a refused channel is gone already
        const char* what = open ? "attach moved on" : refused ? "refused (the hop serves no lane)" : "open timed out";
        LaneFailedLocked(ours, what, {}, refused);
      }
    }
    if (unused) {
      // Outside `mu`: the hop may have bound it (or bind it when its OPEN arrives), and a lane nobody reads loses
      // that member's media. A fresh one replaces it.
      ResetHalfOpenLane(hop, channel_id);
    }
  }

  /** How long a lane open may take before it is tried again. */
  static constexpr std::chrono::seconds kDataLaneOpenTimeout{5};

  /** Retry backoff after the n-th failure in a row: 1, 2, 4, 8, then 10 s. */
  static std::chrono::milliseconds DataLaneRetryDelay(const int failures) {
    const int shift = std::clamp(failures - 1, 0, 3);
    return std::min(std::chrono::milliseconds(1000 << shift), std::chrono::milliseconds(10000));
  }

  /**
   * Requires `mu`. A lane open failed. A hop that serves none (`hop_serves_none`) is no media hop: an attach waiting
   * for its lane fails, an attached session is lost (its observers re-attach elsewhere). Otherwise the io tick tries
   * again after a backoff, media dropped meanwhile. Closing the control channel here resolves the hop's link: the
   * callers run on the io thread (io → mu).
   */
  void LaneFailedLocked(const bool still_ours, const char* what, const std::string& detail,
                        const bool hop_serves_none) {
    if (!still_ours) {
      LaneLog().info << "media_relay data lane: " << what;
      return;
    }
    client_.lane_opening = false;
    if (hop_serves_none) {
      LaneLog().info << "media_relay data lane: " << what << ": not a media hop call_id=" << client_.call_id;
      if (client_.attach_done) {
        FinishPendingAttachLocked(Failure::Of(Err::HopRefused, "media-relay: the hop serves no data lane"));
      } else {
        NotifyClientObserversLocked(MediaRelayClientLoss::TransportLost);
      }
      DetachClientLocked();
      return;
    }
    ++client_.lane_failures;
    const auto delay = DataLaneRetryDelay(client_.lane_failures);
    client_.lane_retry_at = Clock::now() + delay;
    LaneLog().info << "media_relay data lane: " << what << (detail.empty() ? "" : " (" + detail + ")")
                   << ": media dropped until it opens, again in " << delay.count() << " ms (failure "
                   << client_.lane_failures << ")";
  }

  /** On the io thread: reset a lane channel that did not open in time, so the hop drops its end. */
  void ResetHalfOpenLane(const std::string& hop, const uint32_t channel_id) {
    if (pp::amp::PeerLink* link = ResolveLink(hop); link && link->Mux()) {
      (void)link->Mux()->ResetChannel(channel_id);
    }
  }

  /** Io tick, without `mu`: a lane retry that is due. */
  void RetryDataLaneIfDue(const Clock::time_point now) {
    std::string hop;
    std::string call_id;
    {
      std::lock_guard lock(mu);
      if (!client_.channel || client_.data_channel || client_.lane_opening ||
          client_.lane_retry_at == Clock::time_point{} || now < client_.lane_retry_at) {
        return;
      }
      client_.lane_retry_at = {};
      hop = client_.hop_peer_key;
      call_id = client_.call_id;
    }
    OpenDataLane(hop, call_id);
  }

  /** Requires `mu`. */
  void BindDataLaneLocked(const std::string& hop, const uint32_t channel_id) {
    client_.data_channel = runtime->Links().BindChannel(
        hop, channel_id, pp::amp::MediaRelayDataChannelPolicy(),
        [this](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          return HandleClientMediaFrame(*frame);
        },
        // Posted, and bound to `lifetime` (as BindClientChannel's): the lane
        // can end under `mu`, or after we are gone.
        lifetime.Bind([this](const char*) {
          PostIo([this] {
            std::lock_guard lock(mu);
            if (client_.data_channel && client_.data_channel->IsClosed()) {
              client_.data_channel.reset();
              LaneFailedLocked(static_cast<bool>(client_.channel), "lane ended", {}, /*hop_serves_none=*/false);
            }
          });
        }));
  }

  /** Requires `mu`. Attached to a remote hop: its attach reported (its lane opened once). */
  bool RemoteAttachedLocked() const { return client_.channel != nullptr && !client_.attach_done; }

  /** Requires `mu`. Quote reply, or accept → attach replies, on a bundle's channel. */
  bool HandleSessionFrame(const MediaRelaySessionId id, Roe<std::vector<uint8_t>> frame) {
    auto* session = Find(id);
    if (!session) {
      return false;
    }
    if (!frame) {
      TearDown(*session, false, Failure::Of(Err::Closed, "media-relay channel failed"));
      return false;
    }
    auto root = common::TryParseObject(BodyToJson(*frame));
    if (!root) {
      TearDown(*session, false, Failure::Of(Err::BadReply, "invalid media-relay json"));
      return false;
    }
    if (PeerLoadBook* book = loads.load(std::memory_order_acquire)) {
      NoteReplyLoads(*book, session->hop_peer_key, MeshService::MediaRelay, *root, PeerLoadBook::NowMs());
    }
    if (session->role == MediaRelayBundleRole::ClientQuote && session->phase == MediaRelayBundlePhase::WaitQuote) {
      const auto decision =
          DecideMediaRelayQuoteAck({.phase = session->phase, .ack_ok = root->getIf<bool>("ok").value_or(false)});
      if (decision == MediaRelayQuoteAckDecision::IgnoreStale) {
        return true;
      }
      if (decision == MediaRelayQuoteAckDecision::Fail) {
        TearDown(*session, false, Refusal(*root, "quote failed"));
        return false;
      }
      auto quote = ParseQuoteResponse(*root);
      session->phase = MediaRelayBundlePhase::Closing;
      FinishQuote(*session, std::move(quote));
      // A quote channel is one-shot: the attach opens its own.
      if (session->channel) {
        CloseQuietSlot(session->channel, ResolveLink(session->hop_peer_key));
      }
      sessions.erase(id.value);
      return false;
    }
    if (session->role != MediaRelayBundleRole::ClientAttach) {
      return true;
    }
    if (session->phase == MediaRelayBundlePhase::WaitAccept) {
      if (!root->getIf<bool>("ok").value_or(false)) {
        TearDown(*session, false, Refusal(*root, "accept failed"));
        return false;
      }
      session->session_token = root->getString("session_token").value_or("");
      session->phase = MediaRelayBundlePhase::OutboundAttach;
      common::Object attach_req;
      attach_req.set("v", int64_t{1});
      attach_req.set("op", "attach");
      attach_req.set("session_token", session->session_token);
      attach_req.set("call_id", session->call_id);
      attach_req.set("auth", session->auth_stub);
      if (!session->channel->EnqueueOutbound(JsonToBody(common::DumpJson(attach_req)))) {
        TearDown(*session, false, Failure::Of(Err::ChannelFailed, "failed to send attach"));
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
        TearDown(*session, false, Refusal(*root, "attach failed"));
        return false;
      }
      session->phase = MediaRelayBundlePhase::Attached;
      MediaRelayAttachResult out;
      out.ok = true;
      out.session_token = session->session_token;
      // Reported once the data lane opens: the session is no media path before it.
      AttachFinished done = std::move(session->on_attach);
      session->on_attach = nullptr;
      session->finished = true;
      AdoptClientChannel(*session, std::move(done), std::move(out));
      return true;
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
        // Posted: a write that fails closes the channel inside EnqueueOutbound, and several sends
        // run under `mu` (HandleSessionFrame, OpenAndSend) — handling it inline re-locked `mu`.
        // Bound to `lifetime`: the callback can fire after this coordinator is freed (MeshHost
        // frees L4 before Amp tears down its channels).
        lifetime.Bind([this, id](const char*) {
          PostIo([this, id] {
            std::lock_guard lock(mu);
            if (auto* session = Find(id)) {
              const auto decision = DecideMediaRelayBundleClose({
                  .phase = session->phase,
                  .local_cancel = session->local_cancel,
                  .remote_terminal = true,
                  .finished = session->finished,
              });
              if (decision != MediaRelayBundleCloseDecision::Ignore) {
                TearDown(*session, session->local_cancel,
                         session->local_cancel ? Aborted() : Failure::Of(Err::Closed, "media-relay channel closed"));
              }
            }
          });
        }));
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
              TearDown(*session, false, WrapLinkFailure(channel.error()));
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
                  TearDown(*session, false, Failure::Of(Err::ChannelFailed, "media-relay: channel open failed"));
                  return;
                }
                BindClientChannel(*session, hop, channel_id);
                session->phase = wait_phase;
                if (!session->channel || !session->channel->EnqueueOutbound(JsonToBody(json))) {
                  TearDown(*session, false, Failure::Of(Err::ChannelFailed, send_error));
                }
              });
        });
  }

  void BeginQuote(Session& session, const MediaRelayQuoteRequest& request) {
    session.phase = MediaRelayBundlePhase::OutboundQuote;
    common::Object req;
    req.set("v", int64_t{1});
    req.set("op", "quote");
    req.set("call_id", request.session_id);
    req.set("participants", int64_t{request.participants});
    req.set("want_up_bps", request.want_up_bps);
    req.set("want_down_bps", request.want_down_bps);
    req.set("video_levels", VideoLevelsToJson(request.video_levels));
    req.set("video_parallel", int64_t{request.video_parallel});
    const std::string json = common::DumpJson(req);
    OpenAndSend(session, json, MediaRelayBundlePhase::WaitQuote, "failed to send quote");
  }

  void BeginAttach(Session& session) {
    session.phase = MediaRelayBundlePhase::OutboundAccept;
    common::Object accept_req;
    accept_req.set("v", int64_t{1});
    accept_req.set("op", "accept");
    accept_req.set("quote_id", session.quote_id);
    const std::string json = common::DumpJson(accept_req);
    OpenAndSend(session, json, MediaRelayBundlePhase::WaitAccept, "failed to send accept");
  }

  /** Posts `on_finished` its failure to io and returns true when a bundle cannot start. */
  template <typename Finished>
  bool RefuseStart(const std::string& hop_peer_key, Finished& on_finished) {
    std::optional<Failure> why;
    if (!started.load(std::memory_order_acquire)) {
      why = Failure::Of(Err::NotStarted, "media-relay service not started");
    } else if (const auto link = runtime->Links().GetLinkSnapshot(hop_peer_key);
               !link.has_endpoint && link.phase != pp::amp::PeerLinkPhase::Connected) {
      // Neither an address nor a link (a hop behind a circuit is reached by a link nested through it).
      why = Failure::Of(Err::HopNoEndpoint, "hop peer endpoint not registered");
    }
    if (!why) {
      return false;
    }
    if (on_finished) {
      runtime->PostToIo([on_finished = std::move(on_finished), why = *why]() mutable { on_finished(why); });
    }
    return true;
  }
};

MediaRelayClientCoordinator::MediaRelayClientCoordinator(pp::amp::MeshRuntime& runtime, MediaRelayServer* local_server)
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

void MediaRelayClientCoordinator::SetPeerLoadBook(PeerLoadBook* book) {
  impl_->loads.store(book, std::memory_order_release);
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
    cb(Aborted());
  }
  for (auto& cb : attach_cbs) {
    cb(Aborted());
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
  impl_->PostIo(
      [impl = impl_.get(), id, hop_peer_key, request, on_finished = std::move(on_finished), timeout_ms]() mutable {
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

MediaRelaySessionId MediaRelayClientCoordinator::StartAttach(const std::string& hop_peer_key,
                                                             const std::string& quote_id, const std::string& call_id,
                                                             const std::string& auth_stub, FrameHandler on_frame,
                                                             AttachFinished on_finished, const int timeout_ms) {
  if (impl_->started.load(std::memory_order_acquire) && (quote_id.empty() || call_id.empty())) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Failure::Of(Err::BadRequest, "missing quote_id or call_id"));
      });
    }
    return {};
  }
  if (impl_->RefuseStart(hop_peer_key, on_finished)) {
    return {};
  }
  const MediaRelaySessionId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, hop_peer_key, quote_id, call_id, auth_stub, on_frame = std::move(on_frame),
                 on_finished = std::move(on_finished), timeout_ms]() mutable {
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
      impl->TearDown(*session, true, Aborted());
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

MediaRelayRoe<MediaRelayAttachResult> MediaRelayClientCoordinator::AttachAsLocalHop(
    const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) {
  if (call_id.empty()) {
    return Failure::Of(Err::BadRequest, "missing call_id");
  }
  if (!IsStarted() || !impl_->local_server || !impl_->local_server->IsStarted()) {
    return Failure::Of(Err::NotStarted, "media_relay not started");
  }
  const std::string local_peer = runtime_.Links().LocalCapability().local_peer_id;
  if (local_peer.empty()) {
    return Failure::Of(Err::NotStarted, "amp media-relay: missing local peer id");
  }
  // io → mu, as the io tick: replacing a remote attach closes its channels, which resolves the link under the io lock.
  return runtime_.WithIoLock([&]() -> MediaRelayRoe<MediaRelayAttachResult> {
    std::lock_guard lock(impl_->mu);
    if (!impl_->local_server->LocalAttachedTo(call_id, local_peer)) {
      impl_->DetachClientLocked();
    }
    return impl_->local_server->AttachLocal(call_id, local_peer, std::move(on_frame));
  });
}

MediaRelayRoe<void> MediaRelayClientCoordinator::Subscribe(const uint32_t stream_id, const uint16_t channel_id) {
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
      return Failure::Of(Err::NotAttached, "not attached");
    }
    impl_->client_.subscriptions.insert(key);
    channel = impl_->client_.channel;
  }
  common::Object sub;
  sub.set("v", int64_t{1});
  sub.set("op", "subscribe");
  sub.setJsonUInt("stream_id", stream_id);
  sub.setJsonUInt("channel_id", channel_id);
  // Written outside `mu`, under the io lock (see SendFrame).
  const bool sent = runtime_.WithIoLock([&]() { return channel->EnqueueOutbound(JsonToBody(common::DumpJson(sub))); });
  if (!sent) {
    std::lock_guard lock(impl_->mu);
    impl_->client_.subscriptions.erase(key);
    return Failure::Of(Err::NotAttached, "not attached");
  }
  return {};
}

MediaRelayRoe<void> MediaRelayClientCoordinator::SendFrame(const MediaDataFrame& frame) {
  std::shared_ptr<pp::amp::ChannelSession> channel;
  {
    std::lock_guard lock(impl_->mu);
    // The data lane only (best effort: a lost packet costs this frame only). Never the control channel: there a
    // frame is resent and waits behind the ones before it, arriving later and later (V065).
    channel = impl_->client_.data_channel;
    if (!channel && impl_->client_.channel) {
      // Attached to a remote hop, the lane opening or reopening: lost now, never late.
      impl_->lane_wait_drops.fetch_add(1, std::memory_order_relaxed);
      return {};
    }
  }
  if (!channel) {
    // Local hop: the server fans out from our participant (outside `mu`; Fanout locks internally).
    if (impl_->local_server && impl_->local_server->SendLocal(frame)) {
      return {};
    }
    return Failure::Of(Err::NotAttached, "not attached");
  }
  // The channel session is io-affine: senders (the engine's capture thread) enqueue under the
  // runtime io lock, or they race the mesh pump on the same mux (failed writes, SIGSEGV). And not
  // under `mu`: a failed write fails the channel synchronously, and its closed callback
  // (HandleClientTransportLost) takes `mu` — enqueueing under it deadlocked the sending thread
  // and then the mesh pump. Order: io lock → mu, as the io tick.
  const std::vector<uint8_t> body = EncodeMediaDataFrame(frame);
  const bool queued = runtime_.WithIoLock([&]() { return channel->EnqueueOutbound(body); });
  if (!queued) {
    return Failure::Of(Err::NotAttached, "not attached");
  }
  return {};
}

void MediaRelayClientCoordinator::Detach() {
  // Client state is cleared synchronously under `mu` (no deferred raw-this PostIo), but the channels
  // — the control channel and the data lane — close after `mu` is released: closing resolves the hop's
  // link under the mesh runtime lock, and the io tick (TickDeadlines) holds that lock while it takes
  // `mu` — closing under `mu` was a lock-order inversion. The data lane once closed under `mu` and a
  // leaving member hung (lab, 2026-11-03; ADetachOnAnotherThreadClosesTheDataLaneUnderTheIoLock).
  std::shared_ptr<pp::amp::ChannelSession> closing;
  std::shared_ptr<pp::amp::ChannelSession> closing_lane;
  std::string hop;
  {
    std::lock_guard lock(impl_->mu);
    closing = std::move(impl_->client_.channel);
    closing_lane = std::move(impl_->client_.data_channel);
    hop = impl_->client_.hop_peer_key;
    impl_->DetachClientLocked();
    if (closing) {
      impl_->NotifyClientObserversLocked(MediaRelayClientLoss::Detached);
    }
  }
  if (closing || closing_lane) {
    runtime_.WithIoLock([&]() {
      auto* link = impl_->ResolveLink(hop);
      CloseQuietSlot(closing_lane, link);
      CloseQuietSlot(closing, link);
    });
  }
}

bool MediaRelayClientCoordinator::IsAttached() const {
  std::lock_guard lock(impl_->mu);
  return impl_->RemoteAttachedLocked() || (impl_->local_server && impl_->local_server->IsLocalAttached());
}

bool MediaRelayClientCoordinator::IsLocalHopAttached() const {
  std::lock_guard lock(impl_->mu);
  return impl_->local_server && impl_->local_server->IsLocalAttached();
}

double MediaRelayClientCoordinator::PathPressure() const {
  return 0.0;
} // not reported yet (V032)

MediaRelayClientHealth MediaRelayClientCoordinator::HealthSnapshot() const {
  MediaRelayClientHealth health;
  std::string relay_key;
  {
    std::lock_guard lock(impl_->mu);
    health.attached = impl_->RemoteAttachedLocked() || (impl_->local_server && impl_->local_server->IsLocalAttached());
    health.data_lane = impl_->client_.data_channel != nullptr;
    health.lane_wait_drops = impl_->lane_wait_drops.load(std::memory_order_relaxed);
    if (impl_->client_.channel) {
      relay_key = impl_->client_.hop_peer_key;
    }
  }
  if (relay_key.empty()) {
    return health; // detached, or on this device's own hop: no link to a relay
  }
  // Under the link strand only (IO callbacks take `mu` inside it).
  pp::amp::LinkHandle link{};
  runtime_.Links().WithLiveLinkByDialKey(relay_key, [&](pp::amp::PeerLink& live) { link = live.Handle(); });
  if (const auto stats = runtime_.Links().LinkConnectionStats(link)) {
    health.link_available = true;
    health.link_id = link.id.value;
    health.reliable_sent = stats->reliable_sent;
    health.retransmits = stats->retransmits;
    health.srtt_ms = stats->srtt_ms;
  }
  return health;
}

} // namespace pp::mesh
