#include "mesh/l4/media_relay/serve/MediaRelayServer.h"

#include "common/Utilities.h"

#include "mesh/l4/media_relay/MediaRelayErr.h"

#include "mesh/l4/shared/ChannelSessionSlot.h"
#include "mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "mesh/l4/media_relay/MediaRelayBundleLogic.h"
#include "mesh/l4/media_relay/MediaRelayFrames.h"
#include "mesh/l4/media_relay/MediaRelayLogic.h"
#include "mesh/l4/media_relay/MediaRelayVideoLevels.h"
#include "mesh/l4/media_relay/serve/MediaRelayAttachSm.h"
#include "common/ValueJson.h"
#include "common/metrics/MetricsRegistry.h"
#include "common/Lifetime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace pp::mesh {

namespace {

using Clock = std::chrono::steady_clock;
/** See Impl::empty_session_grace. */
constexpr std::chrono::milliseconds kEmptySessionGrace{60000};

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

/** Unguessable (a counter was "s1", "s2", …): it is what lets an accepted channel attach (PB-021). */
std::string MakeSessionToken() {
  return "s-" + util::GenerateUuid();
}

} // namespace

namespace {

/** Operator metrics (pp-browser docs/contracts/NODE_METRICS.md § Media relay). */
struct MediaRelayMetrics {
  MetricCounter& frames_received;
  MetricCounter& bytes_received;
  MetricCounter& frames_forwarded;
  MetricCounter& bytes_forwarded;
  MetricCounter& dropped_stale;
  MetricCounter& dropped_not_carried;
  MetricCounter& dropped_not_attached;
  /** Frames not sent to a participant whose data lane is not open (media never goes on its control channel). */
  MetricCounter& dropped_no_lane;
  /** Media frames that came on a participant's control channel, not its lane: dropped. */
  MetricCounter& dropped_control_channel;
  /** Frames that came on a participant's data lane. */
  MetricCounter& frames_data_lane;
  MetricCounter& quotes_issued;
  MetricCounter& quotes_refused_admission;
  MetricCounter& quotes_refused_video_level;
  MetricCounter& quotes_refused_busy;
  MetricCounter& attaches_ok;
  MetricCounter& attaches_refused;

  /** Its series exist (at 0) from the start. */
  static MediaRelayMetrics In(MetricsRegistry& r) {
    {
      const char* frames = "Media relay data frames.";
      const char* bytes = "Media relay data frame bytes.";
      const char* dropped = "Media relay frames dropped, by reason.";
      const char* quotes = "Media relay quotes, by result.";
      const char* attaches = "Media relay attaches, by result.";
      return MediaRelayMetrics{
          r.Counter("pp_media_relay_frames_total", frames, {{"direction", "received"}}),
          r.Counter("pp_media_relay_bytes_total", bytes, {{"direction", "received"}}),
          r.Counter("pp_media_relay_frames_total", frames, {{"direction", "forwarded"}}),
          r.Counter("pp_media_relay_bytes_total", bytes, {{"direction", "forwarded"}}),
          r.Counter("pp_media_relay_dropped_frames_total", dropped, {{"reason", "stale"}}),
          r.Counter("pp_media_relay_dropped_frames_total", dropped, {{"reason", "not_carried"}}),
          r.Counter("pp_media_relay_dropped_frames_total", dropped, {{"reason", "not_attached"}}),
          r.Counter("pp_media_relay_dropped_frames_total", dropped, {{"reason", "no_lane"}}),
          r.Counter("pp_media_relay_dropped_frames_total", dropped, {{"reason", "control_channel"}}),
          r.Counter("pp_media_relay_lane_frames_total", "Media relay data frames received, by lane.",
                    {{"lane", "data"}}),
          r.Counter("pp_media_relay_quotes_total", quotes, {{"result", "issued"}}),
          r.Counter("pp_media_relay_quotes_total", quotes, {{"result", "refused_admission"}}),
          r.Counter("pp_media_relay_quotes_total", quotes, {{"result", "refused_video_level"}}),
          r.Counter("pp_media_relay_quotes_total", quotes, {{"result", "refused_busy"}}),
          r.Counter("pp_media_relay_attaches_total", attaches, {{"result", "ok"}}),
          r.Counter("pp_media_relay_attaches_total", attaches, {{"result", "refused"}}),
      };
    }
  }
};

} // namespace

struct MediaRelayServer::Impl {
  explicit Impl(MetricsRegistry& registry) : metrics(MediaRelayMetrics::In(registry)) {}

  pp::amp::MeshRuntime* runtime = nullptr;
  /** This mesh's operator metrics (its MeshHost's registry). */
  MediaRelayMetrics metrics;
  mutable std::mutex mu;
  MediaRelayAdmissionPolicy admission;
  MediaRelayVideoPolicy video_policy;
  /** The node's service levels (MeshHost's); null: none said. */
  std::atomic<ServiceLoadMonitor*> load{nullptr};
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<bool> serve_inbound{true};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** PostIo(raw Impl*) — Invalidate on AbortInflight (Stop calls Abort). */
  pp::Lifetime deferred;
  /** IoTick / protocol handler — Invalidate only on Stop (survives mid-life Abort). */
  pp::Lifetime lifetime;

  struct HostParticipant {
    std::string peer_id;
    std::shared_ptr<pp::amp::ChannelSession> channel;
    /**
     * Its data lane (kMediaRelayDataProtocolId) once it opened one: media to
     * and from it goes there. Until then — and for a participant behind a
     * circuit — `channel` carries the media too.
     */
    std::shared_ptr<pp::amp::ChannelSession> data_channel;
    /** Set for the local participant (this node's client): frames go here, not to a channel. */
    FrameHandler local_on_frame;
    std::unordered_set<uint64_t> subscriptions;
    std::unordered_map<uint64_t, uint32_t> last_lossy_seq;
    /** B009: video levels this participant may publish here; nullopt = any (the local participant). */
    std::optional<std::vector<uint8_t>> video_levels;
  };

  struct HostSession {
    std::string call_id;
    std::string session_token;
    std::vector<std::shared_ptr<HostParticipant>> participants;
    /** Set while the session has no participants; it is dropped once the grace has passed. */
    std::optional<Clock::time_point> empty_since;
  };

  /** Quotes issued and not yet accepted — expire / capped. */
  MediaRelayQuoteBook quotes;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> hosts_by_call;
  /** Group calls hosted at most (the operator's capacity; under `mu`). */
  size_t max_sessions = kMediaRelayServeMaxSessions;
  std::shared_ptr<HostParticipant> local_part_;
  std::shared_ptr<HostSession> local_session_;
  std::string local_peer_id_;
  /** Bytes relayed for others: from a remote participant to a remote participant (this server). */
  std::atomic<uint64_t> relayed_bytes{0};

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    deferred.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); }, std::move(task));
  }

  /** The participant's link its channels ride: the Connected one (direct, or nested through a circuit). */
  pp::amp::PeerLink* ResolveLink(const std::string& peer_key) const {
    if (!runtime || peer_key.empty()) {
      return nullptr;
    }
    if (pp::amp::PeerLink* live = runtime->Links().ResolveConnectedLink(peer_key)) {
      return live;
    }
    return runtime->Links().FindLink(peer_key);
  }

  void Tick() {
    std::lock_guard lock(mu);
    const auto now = Clock::now();
    quotes.Expire(now);
    DropEmptySessionsLocked(now);
    // Also between requests: other services' replies say this level ("loads"), and a level only a request moved
    // would stay full after the calls ended, with no one asking again.
    (void)PublishLoadLocked();
  }

  /**
   * How long a session outlives its last participant. Call-scoped admission lets a non-contact
   * join a call while its session exists: when every participant drops at once (the hop's links
   * flap, a path migration), they re-attach within the grace in any order; after it the call is
   * over, and a new session for the call id needs contact admission again.
   */
  std::chrono::milliseconds empty_session_grace{kEmptySessionGrace};
  /** SetResetDataLanesForTest: lanes still to reset as they open. */
  std::atomic<int> reset_lanes_for_test{0};

  /** Requires `mu`. */
  void DropEmptySessionsLocked(const Clock::time_point now) {
    for (auto it = hosts_by_call.begin(); it != hosts_by_call.end();) {
      const auto& session = it->second;
      const bool expired = session && session->participants.empty() && session->empty_since &&
                           now - *session->empty_since >= empty_session_grace;
      it = expired ? hosts_by_call.erase(it) : std::next(it);
    }
  }

  static void EraseParticipant(HostSession& session, const HostParticipant* part) {
    session.participants.erase(
        std::remove_if(session.participants.begin(), session.participants.end(),
                       [part](const std::shared_ptr<HostParticipant>& p) { return p.get() == part; }),
        session.participants.end());
  }

  /**
   * Requires `mu`. Erase `part`; a session left without participants is dropped after the empty
   * grace (DropEmptySessionsLocked) — not the local hop's own session while it is attached.
   */
  void EraseParticipantLocked(const std::shared_ptr<HostSession>& session, const HostParticipant* part) {
    if (part && part->data_channel) {
      // Closed on the io lane, not under `mu`: its closed callback comes back
      // here.
      PostIo([this, channel = part->data_channel, peer = part->peer_id]() mutable {
        CloseQuietSlot(channel, ResolveLink(peer));
      });
    }
    EraseParticipant(*session, part);
    if (session->participants.empty() && session != local_session_ && !session->empty_since) {
      session->empty_since = Clock::now();
    }
  }

  /**
   * A participant's channel ended — the peer closed or reset it, its link dropped, or the peer
   * vanished: release the participant. Amp reports that only to the channel's closed callback
   * (never as a frame error), and a participant that left without `detach` used to stay in its
   * session for the node's lifetime. Posted: the channel can close while `mu` is held (Reject).
   */
  void ReleaseParticipantOf(std::shared_ptr<pp::amp::ChannelSession> channel) {
    if (!runtime || !channel) {
      return;
    }
    lifetime.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); },
                  [this, channel = std::move(channel)] {
                    std::lock_guard lock(mu);
                    std::shared_ptr<HostSession> host;
                    std::shared_ptr<HostParticipant> part;
                    if (FindParticipantByChannel(channel.get(), host, part)) {
                      EraseParticipantLocked(host, part.get());
                    }
                  });
  }

  /** Requires `mu`. */
  void DetachLocalLocked() {
    std::shared_ptr<HostSession> session = local_session_;
    if (local_part_) {
      local_part_->local_on_frame = nullptr;
    }
    const std::shared_ptr<HostParticipant> part = std::move(local_part_);
    local_session_.reset();
    local_peer_id_.clear();
    if (part && session) {
      EraseParticipantLocked(session, part.get());
    }
  }

  /**
   * Fan-out must not run while `mu` is held across callbacks / EnqueueOutbound.
   * Snapshot participants under lock (same pattern as MediaRelayRuntime::Fanout).
   */
  /** `from_remote`: the frame came from a remote participant (not this node's own media). */
  void Fanout(const std::shared_ptr<HostSession>& session, const std::string& from_peer, const bool from_remote,
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
                                        it != part->last_lossy_seq.end() ? it->second : 0, frame.seq, frame.mark)) {
            metrics.dropped_stale.Inc();
            continue;
          }
          part->last_lossy_seq[key] = frame.seq;
        }
        if (part->local_on_frame) {
          on_frame = part->local_on_frame;
        } else if (part->data_channel) {
          channel = part->data_channel; // media rides the lane only, never the control channel
        } else {
          metrics.dropped_no_lane.Inc();
          continue;
        }
      }
      metrics.frames_forwarded.Inc();
      metrics.bytes_forwarded.Inc(body.size());
      if (on_frame) {
        if (frame.payload.empty() && body.size() > kMediaFrameHeaderBytes) {
          // The local participant takes the frame itself; a remote sender's was decoded header-only.
          if (auto full = DecodeMediaDataFrame(body)) {
            on_frame(*full);
          }
        } else {
          on_frame(frame);
        }
        continue;
      }
      if (channel) {
        // From the bytes as they came: with nothing queued ahead the mux encodes straight from them, no copy per member.
        (void)channel->EnqueueOutbound(std::span<const uint8_t>(body));
        if (from_remote) {
          relayed_bytes.fetch_add(body.size(), std::memory_order_relaxed);
        }
      }
    }
  }

  static void SendAck(pp::amp::ChannelSession& channel, const char* op) {
    common::Object ack;
    ack.set("v", int64_t{1});
    ack.set("ok", true);
    ack.set("op", op);
    channel.EnqueueOutbound(JsonToBody(common::DumpJson(ack)));
  }

  /** Requires `mu` held. */
  bool HandleParticipantControl(const std::shared_ptr<HostSession>& session,
                                const std::shared_ptr<HostParticipant>& part, const common::Object& root) {
    const std::string op = root.getString("op").value_or("");
    const uint32_t stream_id = static_cast<uint32_t>(root.getNonNegInt("stream_id").value_or(0));
    const uint16_t channel_id = static_cast<uint16_t>(root.getNonNegInt("channel_id").value_or(0));
    if (op == "subscribe") {
      if (part->subscriptions.size() >= kMediaRelayMaxSubscriptionsPerParticipant &&
          !part->subscriptions.contains(MediaRelaySubKey(stream_id, channel_id))) {
        return true; // full (PB-053): ignored, no ack
      }
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
      EraseParticipantLocked(session, part.get());
      return false;
    }
    return true;
  }

  /** A frame on an attached participant's control channel: JSON control; media there is dropped (lane only). */
  bool HandleParticipantControlFrame(const std::shared_ptr<HostSession>& session,
                                     const std::shared_ptr<HostParticipant>& part, const std::vector<uint8_t>& body) {
    if (body.empty()) {
      return true;
    }
    if (body[0] != '{') {
      metrics.dropped_control_channel.Inc();
      return true;
    }
    auto root = common::TryParseObject(BodyToJson(body));
    if (!root) {
      return true;
    }
    std::lock_guard lock(mu);
    return HandleParticipantControl(session, part, *root);
  }

  /** A media frame on a participant's data lane. Must not be called with `mu` held (Fanout locks internally). */
  bool HandleParticipantMedia(const std::shared_ptr<HostSession>& session, const std::shared_ptr<HostParticipant>& part,
                              const std::vector<uint8_t>& body) {
    auto frame = DecodeMediaDataFrameHeader(body); // forwarded as it came: the payload is not copied
    if (!frame) {
      return true;
    }
    metrics.frames_received.Inc();
    metrics.bytes_received.Inc(body.size());
    if (!CarriesChannelOf(*part, frame->channel_id)) {
      metrics.dropped_not_carried.Inc();
      return true; // a channel this relay did not agree to carry for this participant (B009)
    }
    Fanout(session, part->peer_id, /*from_remote=*/true, *frame, body);
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
        EraseParticipantLocked(session, part.get());
        return false;
      }
      return HandleParticipantControlFrame(session, part, *frame);
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

  /** Requires `mu`. The group-call pressure this server sees (calls hosted, of
   * the limit), then the level. */
  ServiceLoad PublishLoadLocked() {
    ServiceLoadMonitor* monitor = load.load(std::memory_order_acquire);
    if (!monitor) {
      return ServiceLoad::Unknown;
    }
    monitor->SetPressure(MeshService::MediaRelay, "sessions",
                         static_cast<double>(hosts_by_call.size()) / static_cast<double>(max_sessions));
    return monitor->Level(MeshService::MediaRelay);
  }

  ServiceLoad LevelNow() const {
    ServiceLoadMonitor* monitor = load.load(std::memory_order_acquire);
    return monitor ? monitor->Level(MeshService::MediaRelay) : ServiceLoad::Unknown;
  }

  /** A refusal: its code for the client's logic, its text for logs, the node's
   * level (docs/ERRORS.md). */
  void Reject(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const MediaRelayRefusal refusal,
              const std::string& error, const MediaRelayAttachEvent ev) {
    common::Object err;
    err.set("v", int64_t{1});
    err.set("ok", false);
    if (const char* code = MediaRelayRefusalWire(refusal); *code != '\0') {
      err.set("code", std::string(code));
    }
    err.set("error", error);
    SetLoadFields(err, LevelNow(), load.load(std::memory_order_acquire));
    channel.EnqueueOutbound(JsonToBody(common::DumpJson(err)));
    (void)sm.Apply(ev);
    channel.Close();
  }

  /** Requires `mu`: the participant bound to `channel`, searching every hosted session. */
  /** Requires `mu`. The remote participant `peer_id` attached here: its latest
   * attach. */
  bool FindParticipantByPeer(const std::string& peer_id, std::shared_ptr<HostSession>& host,
                             std::shared_ptr<HostParticipant>& part) {
    bool found = false;
    for (auto& [call_id_key, host_session] : hosts_by_call) {
      (void)call_id_key;
      for (const auto& candidate : host_session->participants) {
        if (candidate && !candidate->local_on_frame && candidate->peer_id == peer_id) {
          host = host_session;
          part = candidate;
          found = true;
        }
      }
    }
    return found;
  }

  /** A data lane ended: its participant's media goes back to the control
   * channel. Posted (it can end under `mu`). */
  void ForgetDataChannel(std::shared_ptr<pp::amp::ChannelSession> channel) {
    if (!runtime || !channel) {
      return;
    }
    lifetime.Post([rt = runtime](std::function<void()> t) { rt->PostToIo(std::move(t)); },
                  [this, channel = std::move(channel)] {
                    std::lock_guard lock(mu);
                    for (auto& [call_id_key, host_session] : hosts_by_call) {
                      (void)call_id_key;
                      for (const auto& part : host_session->participants) {
                        if (part && part->data_channel == channel) {
                          part->data_channel.reset();
                        }
                      }
                    }
                  });
  }

  /**
   * A participant opened its data lane: bound to it at once (a viewer only
   * receives, so it may never send on it), and again on each frame (it may open
   * the lane before its attach is recorded). Media only: control stays on the
   * participant's control channel.
   */
  void HandleInboundDataChannel(pp::amp::LinkHandle handle, const std::string& remote_peer_id,
                                const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || remote_peer_id.empty()) {
      return;
    }
    if (reset_lanes_for_test.load(std::memory_order_relaxed) > 0) {
      reset_lanes_for_test.fetch_sub(1, std::memory_order_relaxed);
      (void)runtime->Links().WithLiveLink(handle, [channel_id](pp::amp::PeerLink& link) {
        if (link.Mux()) {
          (void)link.Mux()->ResetChannel(channel_id);
        }
      });
      return;
    }
    auto channel_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *channel_holder = runtime->Links().BindChannelOnLink(
        handle, channel_id, pp::amp::MediaRelayDataChannelPolicy(),
        [this, channel_holder, remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto channel = *channel_holder;
          if (!channel || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          if (frame->empty() || frame->front() == '{') {
            return true;
          }
          std::shared_ptr<HostSession> host;
          std::shared_ptr<HostParticipant> part;
          {
            std::lock_guard lock(mu);
            if (!FindParticipantByPeer(remote_peer_id, host, part)) {
              metrics.dropped_not_attached.Inc();
              return true;
            }
            part->data_channel = channel;
          }
          metrics.frames_data_lane.Inc();
          return HandleParticipantMedia(host, part, *frame);
        },
        [forget = lifetime.Bind(
             [this](std::shared_ptr<pp::amp::ChannelSession> channel) { ForgetDataChannel(std::move(channel)); }),
         channel_holder](const char* /*reason*/) {
          forget(*channel_holder);
          channel_holder->reset();
        });
    std::lock_guard lock(mu);
    std::shared_ptr<HostSession> host;
    std::shared_ptr<HostParticipant> part;
    if (*channel_holder && FindParticipantByPeer(remote_peer_id, host, part)) {
      part->data_channel = *channel_holder;
    }
  }

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

  /**
   * B009: a participant that negotiated levels sends audio and video of those levels only — any
   * other channel (another level, a reserved kind) is dropped at ingest. Without a negotiation (a
   * call's hop) every channel passes. Reads the attach-time levels, fixed before the participant is
   * published (no `mu`).
   */
  static bool CarriesChannelOf(const HostParticipant& part, const uint16_t channel_id) {
    if (!part.video_levels) {
      return true;
    }
    if (IsAudioChannel(channel_id)) {
      return true;
    }
    if (!IsVideoChannel(channel_id)) {
      return false;
    }
    const uint8_t level = VideoLevelOf(channel_id);
    return std::find(part.video_levels->begin(), part.video_levels->end(), level) != part.video_levels->end();
  }

  /** Requires `mu`. */
  bool HandleQuote(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const common::Object& root,
                   const MediaRelayOpAdmitContext& admit) {
    const auto decision = DecideMediaRelayOpAdmit(admit);
    if (decision != MediaRelayOpAdmitDecision::Allow) {
      metrics.quotes_refused_admission.Inc();
      if (decision == MediaRelayOpAdmitDecision::RefuseStranger) {
        Reject(channel, sm, MediaRelayRefusal::Refused, "prefer contacts: stranger refused",
               MediaRelayAttachEvent::AdmitFail);
      } else {
        Reject(channel, sm, MediaRelayRefusal::NotReady, "media-relay not ready", MediaRelayAttachEvent::AdmitFail);
      }
      return false;
    }
    MediaRelayQuoteRequest req;
    req.session_id = root.getString("call_id").value_or("");
    req.participants = static_cast<int>(root.getNonNegInt("participants").value_or(1));
    req.want_up_bps = root.getIf<int64_t>("want_up_bps").value_or(0);
    req.want_down_bps = root.getIf<int64_t>("want_down_bps").value_or(0);
    req.video_levels = VideoLevelsFromJson(root, "video_levels");
    req.video_parallel = static_cast<int>(root.getNonNegInt("video_parallel").value_or(1));
    sm.call_id = req.session_id;
    auto carried = ChooseCarriedVideoLevels({req.video_levels, req.video_parallel}, video_policy);
    if (!carried) {
      metrics.quotes_refused_video_level.Inc();
      Reject(channel, sm, MediaRelayRefusal::BadRequest, carried.error().message, MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    // Full or draining: no new call here (one it hosts still takes its members
    // — refusing them would split it).
    if (const ServiceLoad level = PublishLoadLocked();
        ServiceLoadRefusesNew(level) && !hosts_by_call.contains(req.session_id)) {
      metrics.quotes_refused_busy.Inc();
      Reject(channel, sm, MediaRelayRefusal::Busy, std::string("media-relay busy: ") + ServiceLoadWire(level),
             MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    auto q = BuildDefaultMediaRelayQuote(req);
    q.video_levels = *carried;
    if (!quotes.Add(q, req.session_id, sm.remote, Clock::now())) {
      metrics.quotes_refused_busy.Inc();
      Reject(channel, sm, MediaRelayRefusal::Busy, "media-relay busy", MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    metrics.quotes_issued.Inc();
    common::Object quote_resp;
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
    quote_resp.set("video_levels", VideoLevelsToJson(q.video_levels));
    SetLoadFields(quote_resp, LevelNow(), load.load(std::memory_order_acquire));
    channel.EnqueueOutbound(JsonToBody(common::DumpJson(quote_resp)));
    (void)sm.Apply(MediaRelayAttachEvent::OpQuote);
    return true;
  }

  /** Requires `mu`. */
  bool HandleAccept(pp::amp::ChannelSession& channel, MediaRelayAttachSm& sm, const common::Object& root,
                    MediaRelayOpAdmitContext admit) {
    if (sm.phase != MediaRelayAttachPhase::Control && sm.phase != MediaRelayAttachPhase::Quoted) {
      Reject(channel, sm, MediaRelayRefusal::BadRequest, "accept not allowed in phase",
             MediaRelayAttachEvent::OpAccept);
      return false;
    }
    const std::string quote_id = root.getString("quote_id").value_or("");
    const auto now = Clock::now();
    const MediaRelayQuoteBook::Entry* pending = quotes.Find(quote_id, now);
    if (!pending) {
      Reject(channel, sm, MediaRelayRefusal::UnknownQuote, "unknown quote", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    // Admit before consuming: a refused accept leaves the quote for a retry to be refused the same way.
    admit.call_id = pending->call_id;
    admit.session_exists_for_call = hosts_by_call.contains(admit.call_id);
    if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
      Reject(channel, sm, MediaRelayRefusal::Refused, "prefer contacts: stranger refused",
             MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    sm.call_id = pending->call_id;
    sm.video_levels = pending->quote.video_levels;
    quotes.Take(quote_id, now);
    sm.accepted_quote_id = quote_id;
    sm.session_token = MakeSessionToken();
    common::Object accept_resp;
    accept_resp.set("v", int64_t{1});
    accept_resp.set("ok", true);
    accept_resp.set("op", "accept");
    accept_resp.set("session_token", sm.session_token);
    accept_resp.set("quote_id", sm.accepted_quote_id);
    SetLoadFields(accept_resp, LevelNow(), load.load(std::memory_order_acquire));
    channel.EnqueueOutbound(JsonToBody(common::DumpJson(accept_resp)));
    (void)sm.Apply(MediaRelayAttachEvent::OpAccept);
    return true;
  }

  /** Requires `mu`. */
  bool HandleAttach(const std::shared_ptr<pp::amp::ChannelSession>& channel, MediaRelayAttachSm& sm,
                    const common::Object& root, MediaRelayOpAdmitContext admit) {
    if (!sm.Apply(MediaRelayAttachEvent::OpAttach)) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::BadRequest, "attach not allowed in phase",
             MediaRelayAttachEvent::OpAttach);
      return false;
    }
    const std::string token = root.getString("session_token").value_or("");
    const std::string call_id = root.getString("call_id").value_or("");
    const std::string auth = root.getString("auth").value_or("");
    sm.call_id = call_id;
    if (token.empty() || call_id.empty()) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::BadRequest, "missing session_token or call_id",
             MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (sm.session_token.empty() || token != sm.session_token) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::AuthFailed, "session_token is not this channel's",
             MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (!MediaRelayAuthStubOk(auth, call_id)) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::AuthFailed, "auth failed", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    admit.call_id = call_id;
    admit.session_exists_for_call = hosts_by_call.contains(call_id);
    if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::Refused, "prefer contacts: stranger refused",
             MediaRelayAttachEvent::AdmitFail);
      return false;
    }
    std::shared_ptr<HostSession> host = hosts_by_call.contains(call_id) ? hosts_by_call[call_id] : nullptr;
    if (const ServiceLoad level = PublishLoadLocked(); !host && ServiceLoadRefusesNew(level)) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::Busy, std::string("relay busy: ") + ServiceLoadWire(level),
             MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (!host && !MediaRelayCanOpenHostSession(hosts_by_call.size(), max_sessions)) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::Busy, "relay busy: too many sessions", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (host && !MediaRelayCanAddParticipant(host->participants.size(), kMediaRelayServeMaxParticipants)) {
      metrics.attaches_refused.Inc();
      Reject(*channel, sm, MediaRelayRefusal::Busy, "relay busy: session full", MediaRelayAttachEvent::AttachFail);
      return false;
    }
    if (!host) {
      host = std::make_shared<HostSession>();
      host->call_id = call_id;
      host->session_token = token;
      hosts_by_call[call_id] = host;
    }
    auto part = std::make_shared<HostParticipant>();
    part->peer_id = sm.remote;
    part->channel = channel;
    part->video_levels = sm.video_levels;
    host->participants.push_back(part);
    host->empty_since.reset();
    metrics.attaches_ok.Inc();
    PostIo([this, host, part] { RebindParticipantHandlers(host, part); });
    SendAck(*channel, "attach");
    (void)sm.Apply(MediaRelayAttachEvent::AttachOk);
    return true;
  }

  /** Control JSON on an inbound channel: quote → accept → attach, then participant control. */
  bool HandleInboundControl(const std::shared_ptr<pp::amp::ChannelSession>& channel, MediaRelayAttachSm& sm,
                            const std::vector<uint8_t>& frame) {
    auto root = common::TryParseObject(BodyToJson(frame));
    if (!root) {
      Reject(*channel, sm, MediaRelayRefusal::BadRequest, "invalid media-relay json", MediaRelayAttachEvent::Cancel);
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
    Reject(*channel, sm, MediaRelayRefusal::BadRequest, "unsupported op", MediaRelayAttachEvent::OpUnsupported);
    return false;
  }

  void HandleInboundChannel(pp::amp::LinkHandle handle, const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || remote_peer_id.empty()) {
      return;
    }
    auto channel_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto sm = std::make_shared<MediaRelayAttachSm>();
    sm->remote = remote_peer_id;
    (void)sm->Apply(MediaRelayAttachEvent::StreamOpened);

    *channel_holder = runtime->Links().BindChannelOnLink(
        handle, channel_id, pp::amp::MediaRelayClientChannelPolicy(),
        [this, channel_holder, sm](Roe<std::vector<uint8_t>> frame) {
          auto channel = *channel_holder;
          if (!channel || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          if (frame->empty()) {
            return true;
          }
          if (frame->front() != '{') {
            metrics.dropped_control_channel.Inc(); // media rides the data lane only
            return true;
          }
          return HandleInboundControl(channel, *sm, *frame);
        },
        // The handler (owned by the channel) captures this holder, which owns the channel: drop the
        // holder's reference when the channel ends, or every served session leaks (LeakSanitizer).
        // An attached participant on this channel is released too (ReleaseParticipantOf). The
        // callback can fire after this server is freed (MeshHost::Stop frees L4 before Amp tears
        // down channels still mid-handshake): reach Impl only through the lifetime token, which
        // outlives it and goes dead in Stop.
        [release = lifetime.Bind(
             [this](std::shared_ptr<pp::amp::ChannelSession> channel) { ReleaseParticipantOf(std::move(channel)); }),
         channel_holder](const char* /*reason*/) {
          release(*channel_holder);
          channel_holder->reset();
        });
  }
};

MediaRelayServer::MediaRelayServer(pp::amp::MeshRuntime& runtime, MetricsRegistry& metrics)
    : impl_(std::make_unique<Impl>(metrics)), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

void MediaRelayServer::SetEmptySessionGraceForTest(const std::chrono::milliseconds grace) {
  std::lock_guard lock(impl_->mu);
  impl_->empty_session_grace = grace;
}

void MediaRelayServer::SetResetDataLanesForTest(const int count) {
  impl_->reset_lanes_for_test.store(count, std::memory_order_relaxed);
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
      impl_->lifetime.Bind(
          [impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id, const uint32_t ch) {
            impl->HandleInboundChannel(handle, remote_peer_id, ch);
          }));
  runtime_.Links().SetProtocolHandler(
      kMediaRelayDataProtocolId,
      impl_->lifetime.Bind(
          [impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& remote_peer_id, const uint32_t ch) {
            impl->HandleInboundDataChannel(handle, remote_peer_id, ch);
          }));
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
  runtime_.Links().RemoveProtocolHandler(kMediaRelayDataProtocolId);
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

MediaRelayRuntimeStats MediaRelayServer::RuntimeStats() const {
  MediaRelayRuntimeStats stats;
  stats.bytes_relayed = impl_->relayed_bytes.load(std::memory_order_relaxed);
  std::lock_guard lock(impl_->mu);
  for (const auto& [call_id, host] : impl_->hosts_by_call) {
    if (host && !host->participants.empty()) {
      ++stats.active_sessions;
      stats.active_participants += host->participants.size();
      for (const auto& part : host->participants) {
        stats.data_lane_participants += part && part->data_channel ? 1 : 0;
      }
    }
  }
  return stats;
}

void MediaRelayServer::SetLoadMonitor(ServiceLoadMonitor* monitor) {
  impl_->load.store(monitor, std::memory_order_release);
}

void MediaRelayServer::SetMaxSessions(const size_t max_sessions) {
  if (max_sessions > 0) {
    std::lock_guard lock(impl_->mu);
    impl_->max_sessions = max_sessions;
  }
}

void MediaRelayServer::SetVideoPolicy(MediaRelayVideoPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->video_policy = std::move(policy);
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

MediaRelayRoe<MediaRelayAttachResult> MediaRelayServer::AttachLocal(const std::string& call_id,
                                                                    const std::string& local_peer_id,
                                                                    FrameHandler on_frame) {
  if (call_id.empty()) {
    return MediaRelayFailure::Of(MediaRelayErr::BadRequest, "missing call_id");
  }
  if (local_peer_id.empty()) {
    return MediaRelayFailure::Of(MediaRelayErr::NotStarted, "amp media-relay: missing local peer id");
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
  session->empty_since.reset();
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
  impl_->metrics.frames_received.Inc();
  impl_->metrics.bytes_received.Inc(body.size());
  runtime_.WithIoLock([&]() { impl_->Fanout(session, from_peer, /*from_remote=*/false, frame, body); });
  return true;
}

} // namespace pp::mesh
