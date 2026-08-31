#pragma once

#include "base/mesh/channel/ChannelSession.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/p2p/MediaRelayBundleLogic.h"
#include "base/p2p/MediaRelayService.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

/**
 * Non-blocking `/pp-browser/media-relay/1.0.0` quote + attach on MeshRuntime ([A022]).
 * MeshHost owns a parallel instance when Amp is up; SoftMigrate still uses MediaRelayService
 * ([A020]) until Amp Subscribe/SendFrame/local-hop lands.
 * D7b: quote/accept/attach handshake only; fan-out SoftMigrate deferred.
 */
class AmpMediaRelayCoordinator {
public:
  using QuoteFinished = std::function<void(Roe<MediaRelayQuote>)>;
  using AttachFinished = std::function<void(Roe<MediaRelayAttachResult>)>;
  using FrameHandler = std::function<void(MediaDataFrame)>;

  explicit AmpMediaRelayCoordinator(amp::MeshRuntime& runtime);
  ~AmpMediaRelayCoordinator();

  AmpMediaRelayCoordinator(const AmpMediaRelayCoordinator&) = delete;
  AmpMediaRelayCoordinator& operator=(const AmpMediaRelayCoordinator&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);

  MediaRelaySessionId StartQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                 QuoteFinished on_finished, int timeout_ms = 8000);

  MediaRelaySessionId StartAttach(const std::string& hop_peer_key, const std::string& quote_id,
                                  const std::string& call_id, const std::string& auth_stub,
                                  FrameHandler on_frame, AttachFinished on_finished,
                                  int timeout_ms = 8000);

  void Cancel(MediaRelaySessionId id);
  void AbortInflight();

  MediaRelayBundlePhase Phase(MediaRelaySessionId id) const;
  bool IsSessionActive(MediaRelaySessionId id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::MeshRuntime& runtime_;
};

} // namespace pbr
