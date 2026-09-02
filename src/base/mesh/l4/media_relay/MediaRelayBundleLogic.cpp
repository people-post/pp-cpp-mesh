#include "base/mesh/l4/media_relay/MediaRelayBundleLogic.h"

#include "base/mesh/l4/media_relay/MediaRelayLogic.h"

#include <algorithm>
#include <atomic>
#include <sstream>

namespace pbr {
namespace {

constexpr int64_t kDefaultUserUpBps = 500'000;
constexpr int64_t kDefaultUserDownBps = 2'000'000;
constexpr int64_t kDefaultSessionUpBps = 4'000'000;
constexpr int64_t kDefaultSessionDownBps = 8'000'000;
constexpr int64_t kDefaultCeilingBytes = 50'000'000;

std::string MakeQuoteId() {
  static std::atomic<uint64_t> seq{1};
  std::ostringstream oss;
  oss << "q" << seq.fetch_add(1, std::memory_order_relaxed);
  return oss.str();
}

} // namespace

MediaRelayOpAdmitDecision DecideMediaRelayOpAdmit(const MediaRelayOpAdmitContext& ctx) {
  if (!ctx.service_started || ctx.stopping) {
    return MediaRelayOpAdmitDecision::RefuseNotReady;
  }
  if (ctx.op != "quote" && ctx.op != "accept" && ctx.op != "attach") {
    return MediaRelayOpAdmitDecision::RefuseBadOp;
  }
  const bool contact_ok =
      RelayAdmissionAllowsDialer(ctx.serve_scope_mask, ctx.dialer_peer_id, ctx.contact_peer_ids);
  if (!MediaRelayCallScopedAdmit(ctx.session_exists_for_call, contact_ok)) {
    return MediaRelayOpAdmitDecision::RefuseStranger;
  }
  return MediaRelayOpAdmitDecision::Allow;
}

MediaRelayQuoteAckDecision DecideMediaRelayQuoteAck(const MediaRelayQuoteAckContext& ctx) {
  if (ctx.phase != MediaRelayBundlePhase::WaitQuote) {
    return MediaRelayQuoteAckDecision::IgnoreStale;
  }
  return ctx.ack_ok ? MediaRelayQuoteAckDecision::Succeed : MediaRelayQuoteAckDecision::Fail;
}

MediaRelayAttachAckDecision DecideMediaRelayAttachAck(const MediaRelayAttachAckContext& ctx) {
  if (ctx.phase != MediaRelayBundlePhase::WaitAttachAck) {
    return MediaRelayAttachAckDecision::IgnoreStale;
  }
  return ctx.ack_ok ? MediaRelayAttachAckDecision::EnterAttached : MediaRelayAttachAckDecision::Fail;
}

MediaRelayBundleCloseDecision DecideMediaRelayBundleClose(const MediaRelayBundleCloseContext& ctx) {
  if (ctx.finished || ctx.phase == MediaRelayBundlePhase::Idle ||
      ctx.phase == MediaRelayBundlePhase::Closing) {
    return MediaRelayBundleCloseDecision::Ignore;
  }
  if (ctx.local_cancel) {
    return MediaRelayBundleCloseDecision::SuppressNotify;
  }
  if (ctx.remote_terminal || MediaRelayBundlePhaseIsActive(ctx.phase)) {
    return MediaRelayBundleCloseDecision::FailSession;
  }
  return MediaRelayBundleCloseDecision::Ignore;
}

bool MediaRelayBundlePhaseIsActive(const MediaRelayBundlePhase phase) {
  switch (phase) {
  case MediaRelayBundlePhase::OutboundQuote:
  case MediaRelayBundlePhase::WaitQuote:
  case MediaRelayBundlePhase::OutboundAccept:
  case MediaRelayBundlePhase::WaitAccept:
  case MediaRelayBundlePhase::OutboundAttach:
  case MediaRelayBundlePhase::WaitAttachAck:
  case MediaRelayBundlePhase::Attached:
  case MediaRelayBundlePhase::HostServe:
    return true;
  case MediaRelayBundlePhase::Idle:
  case MediaRelayBundlePhase::Closing:
    return false;
  }
  return false;
}

MediaRelayQuote BuildDefaultMediaRelayQuote(const MediaRelayQuoteRequest& req, const double rate,
                                            const std::string& mode) {
  MediaRelayQuote q;
  q.ok = true;
  q.quote_id = MakeQuoteId();
  q.a_up_bps = kDefaultUserUpBps;
  q.a_down_bps = kDefaultUserDownBps;
  q.b_up_bps = kDefaultSessionUpBps;
  q.b_down_bps = kDefaultSessionDownBps;
  if (req.want_up_bps > 0) {
    q.a_up_bps = std::min(q.a_up_bps, req.want_up_bps);
  }
  if (req.want_down_bps > 0) {
    q.a_down_bps = std::min(q.a_down_bps, req.want_down_bps);
  }
  q.rate = (mode == "volunteer") ? 0.0 : rate;
  q.pricing_mode = (q.rate <= 0.0) ? "volunteer" : (mode.empty() ? "paid" : mode);
  q.ceiling_bytes = kDefaultCeilingBytes;
  q.ceiling_amount = 0.0;
  return q;
}

} // namespace pbr
