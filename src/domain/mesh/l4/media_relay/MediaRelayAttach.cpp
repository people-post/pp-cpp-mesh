#include "domain/mesh/l4/media_relay/MediaRelayAttach.h"

#include "foundation/runtime/AppRuntime.h"

#include "common/Logger.h"

#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int kQuoteTimeoutMs = 5000;
constexpr int kAttachTimeoutMs = 8000;

logging::Logger& AttachLog() {
  static logging::Logger log = logging::getLogger("MediaRelayAttach");
  return log;
}

/** Off the IO strand (quote / attach take the relay client's lock): a worker, or inline without a runtime. */
void PostOffIo(std::function<void()> task) {
  if (AppRuntime::IsRunning()) {
    AppRuntime::PostWorkerNormal(std::move(task));
  } else {
    task();
  }
}

void QuoteThenAttach(MediaRelayAttachPorts ports, MediaRelayAttachRequest request, MediaRelayAttachHooks hooks,
                     std::function<void(Roe<MediaRelayAttached>)> on_done) {
  if (!ports.dial->IsDialable(request.hop_peer_id)) {
    on_done(Error("hop not dialable"));
    return;
  }
  const std::string hop = request.hop_peer_id;
  const MediaRelayQuoteRequest quote_request = request.quote;
  ports.relay->RequestQuoteAsync(
      hop, quote_request,
      [ports, request = std::move(request), hooks = std::move(hooks),
       on_done = std::move(on_done)](Roe<MediaRelayQuote> quote) mutable {
        if (!quote || !quote->ok) {
          on_done(Error(quote ? quote->error : quote.error().message));
          return;
        }
        if (hooks.accept_quote) {
          if (auto accepted = hooks.accept_quote(*quote); !accepted) {
            AttachLog().info << "quote not accepted hop=" << request.hop_peer_id << " rate=" << quote->rate
                             << " err=" << accepted.error().message;
            on_done(accepted.error());
            return;
          }
        }
        if (hooks.still_wanted && !hooks.still_wanted()) {
          on_done(Error("attach aborted"));
          return;
        }
        MediaRelayAttached attached;
        attached.hop_peer_id = request.hop_peer_id;
        attached.quote_id = quote->quote_id;
        attached.a_up_bps = quote->a_up_bps;
        ports.relay->AcceptAndAttachAsync(
            request.hop_peer_id, attached.quote_id, request.session_id, request.auth, std::move(hooks.on_frame),
            [attached, on_done = std::move(on_done)](Roe<MediaRelayAttachResult> result) mutable {
              if (!result || !result->ok) {
                on_done(Error(result ? result->error : result.error().message));
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

} // namespace

void AttachToMediaRelayAsync(const MediaRelayAttachPorts& ports, MediaRelayAttachRequest request,
                             MediaRelayAttachHooks hooks, std::function<void(Roe<MediaRelayAttached>)> on_done) {
  if (!on_done) {
    return;
  }
  if (!ports.relay || !ports.dial) {
    on_done(Error("media_relay not available"));
    return;
  }
  if (request.hop_peer_id.empty()) {
    on_done(Error("missing hop_peer_id"));
    return;
  }
  if (!request.hop_multiaddr.empty()) {
    (void)ports.dial->RegisterEndpoint(request.hop_peer_id, request.hop_multiaddr);
    ports.dial->ClearDialBackoff(request.hop_peer_id);
  }
  if (!ports.dial->IsDialable(request.hop_peer_id) && ports.service_reach) {
    // Service reach finishes on Amp IO — continue on a worker, not the IO strand.
    const std::string hop = request.hop_peer_id;
    ports.service_reach->TryEnsureHopReachableAsync(
        hop, [ports, request = std::move(request), hooks = std::move(hooks),
              on_done = std::move(on_done)](Roe<void>) mutable {
          PostOffIo([ports, request = std::move(request), hooks = std::move(hooks),
                            on_done = std::move(on_done)]() mutable {
            QuoteThenAttach(ports, std::move(request), std::move(hooks), std::move(on_done));
          });
        });
    return;
  }
  QuoteThenAttach(ports, std::move(request), std::move(hooks), std::move(on_done));
}

} // namespace pbr
