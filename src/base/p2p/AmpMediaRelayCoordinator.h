#pragma once

#include "base/mesh/channel/ChannelSession.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/media/CallMediaHealth.h"
#include "base/p2p/AmpCircuitHopRegistry.h"
#include "base/p2p/MediaRelayBundleLogic.h"
#include "base/p2p/MediaRelayService.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

/**
 * Non-blocking `/pp-browser/media-relay/1.0.0` on MeshRuntime ([A022]).
 * MeshHost owns an instance when Amp is up. SoftMigrate uses AmpMediaRelayClient ([A020]).
 * Circuit-backed hops adopt sessions from AmpCircuitHopRegistry (D9 step 5c).
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

  /** When false, inbound protocol handler refuses new dials (outbound client still works). */
  void SetServeInbound(bool serve);
  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);
  void SetCircuitHopRegistry(AmpCircuitHopRegistry* hops);

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

  void StartClientFrameReader();
  void SetClientTransportLostHandler(std::function<void()> handler);
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& call_id,
                                               std::function<void(MediaDataFrame)> on_frame);
  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> SendFrame(const MediaDataFrame& frame);
  void Detach();
  bool IsAttached() const;
  bool IsLocalHopAttached() const;
  double PathPressure() const;
  CallHopHealth HealthSnapshot() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::MeshRuntime& runtime_;
};

} // namespace pbr
