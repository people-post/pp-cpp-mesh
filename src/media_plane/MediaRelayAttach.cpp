#include "mesh/media_plane/MediaRelayAttach.h"


#include "common/Logger.h"

#include <memory>
#include <utility>
namespace pp::mesh {
namespace {

constexpr int kQuoteTimeoutMs = 5000;
constexpr int kAttachTimeoutMs = 8000;

logging::Logger& AttachLog() {
  static logging::Logger log = logging::getLogger("MediaRelayAttach");
  return log;
}

using Err = MediaRelayErr;
using Failure = MediaRelayFailure;

Failure PortsGoneError() {
  return Failure::Of(Err::Aborted, "mesh media stopped");
}

Failure HopNotDialable() {
  return Failure::Of(Err::HopNoEndpoint, "hop not dialable");
}

void QuoteThenAttach(MediaRelayAttachPorts ports, MediaRelayAttachRequest request, MediaRelayAttachHooks hooks,
                     std::function<void(MediaRelayRoe<MediaRelayAttached>)> on_done) {
  if (!ports.Live()) {
    on_done(PortsGoneError());
    return;
  }
  if (!ports.dial->IsDialable(request.hop_peer_id)) {
    on_done(HopNotDialable());
    return;
  }
  const std::string hop = request.hop_peer_id;
  const MediaRelayQuoteRequest quote_request = request.quote;
  ports.relay->RequestQuoteAsync(
      hop, quote_request,
      [ports, request = std::move(request), hooks = std::move(hooks),
       on_done = std::move(on_done)](MediaRelayRoe<MediaRelayQuote> quote) mutable {
        if (!quote) {
          on_done(quote.error());
          return;
        }
        if (!quote->ok) {
          on_done(Failure::Of(Err::BadReply, quote->error.empty() ? "quote not ok" : quote->error));
          return;
        }
        if (hooks.accept_quote) {
          if (auto accepted = hooks.accept_quote(*quote); !accepted) {
            AttachLog().info << "quote not accepted hop=" << request.hop_peer_id << " rate=" << quote->rate
                             << " err=" << accepted.error().message;
            on_done(Failure::Of(Err::QuoteDeclined, accepted.error().message));
            return;
          }
        }
        if (hooks.still_wanted && !hooks.still_wanted()) {
          on_done(Failure::Of(Err::Aborted, "attach aborted"));
          return;
        }
        if (!ports.Live()) {
          on_done(PortsGoneError());
          return;
        }
        MediaRelayAttached attached;
        attached.hop_peer_id = request.hop_peer_id;
        attached.quote_id = quote->quote_id;
        attached.a_up_bps = quote->a_up_bps;
        attached.video_levels = quote->video_levels;
        ports.relay->AcceptAndAttachAsync(
            request.hop_peer_id, attached.quote_id, request.session_id, request.auth, std::move(hooks.on_frame),
            [attached, on_done = std::move(on_done)](MediaRelayRoe<MediaRelayAttachResult> result) mutable {
              if (!result) {
                on_done(result.error());
                return;
              }
              if (!result->ok) {
                on_done(Failure::Of(Err::BadReply, result->error.empty() ? "attach not ok" : result->error));
                return;
              }
              AttachLog().info << "AcceptAndAttach ok hop=" << attached.hop_peer_id
                               << " quote=" << attached.quote_id;
              on_done(std::move(attached));
            },
            kAttachTimeoutMs);
      },
      kQuoteTimeoutMs);
}

/**
 * Register the hop hint and make its service reachable (circuit when not dialable), then `next` off
 * the IO strand. Registration is posted to Amp IO: quoting right after it would find no endpoint.
 */
void ReachHopService(const MediaRelayAttachPorts& ports, const MediaRelayAttachRequest& request,
                     std::function<void()> next) {
  if (!request.hop_multiaddr.empty()) {
    (void)ports.dial->RegisterEndpoint(request.hop_peer_id, request.hop_multiaddr);
    ports.dial->ClearDialBackoff(request.hop_peer_id);
  }
  if (!ports.dial->IsDialable(request.hop_peer_id) && ports.service_reach) {
    // Service reach finishes on Amp IO; quote / attach take the relay client's lock, so continue on
    // the mesh's `work` executor, not the IO strand.
    ports.service_reach->TryEnsureHopReachableAsync(
        request.hop_peer_id,
        [work = ports.work, next = std::move(next)](CircuitReachRoe) mutable { PostOrRun(work, std::move(next)); });
    return;
  }
  next();
}

} // namespace

void AttachToMediaRelayAsync(const MediaRelayAttachPorts& ports, MediaRelayAttachRequest request,
                             MediaRelayAttachHooks hooks,
                             std::function<void(MediaRelayRoe<MediaRelayAttached>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!ports.relay || !ports.dial) {
    on_done(Failure::Of(Err::NotStarted, "media_relay not available"));
    return;
  }
  if (request.hop_peer_id.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing hop_peer_id"));
    return;
  }
  const MediaRelayAttachRequest reach_request = request;
  ReachHopService(ports, reach_request, [ports, request = std::move(request), hooks = std::move(hooks),
                                         on_done = std::move(on_done)]() mutable {
    QuoteThenAttach(ports, std::move(request), std::move(hooks), std::move(on_done));
  });
}

void QuoteMediaRelayAsync(const MediaRelayAttachPorts& ports, MediaRelayAttachRequest request,
                          std::function<void(MediaRelayRoe<MediaRelayQuote>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!ports.relay || !ports.dial) {
    on_done(Failure::Of(Err::NotStarted, "media_relay not available"));
    return;
  }
  if (request.hop_peer_id.empty()) {
    on_done(Failure::Of(Err::BadRequest, "missing hop_peer_id"));
    return;
  }
  const MediaRelayAttachRequest reach_request = request;
  ReachHopService(ports, reach_request, [ports, request = std::move(request), on_done = std::move(on_done)]() mutable {
    if (!ports.Live()) {
      on_done(PortsGoneError());
      return;
    }
    if (!ports.dial->IsDialable(request.hop_peer_id)) {
      on_done(HopNotDialable());
      return;
    }
    ports.relay->RequestQuoteAsync(request.hop_peer_id, request.quote, std::move(on_done), kQuoteTimeoutMs);
  });
}

} // namespace pp::mesh
