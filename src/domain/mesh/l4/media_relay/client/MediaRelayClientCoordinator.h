#pragma once

#include "amp/link/MeshRuntime.h"
#include "common/media/CallMediaHealth.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/media_relay/MediaRelayBundleLogic.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/l4/media_relay/client/IMediaRelayClient.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

/**
 * Client side of `/pp-browser/datagram-relay/1.0.0` on MeshRuntime ([A022]): outbound quote and
 * accept → attach bundles to a hop, then the one attached client session (subscribe / send /
 * frames / loss observers). Circuit-backed hops adopt sessions from AmpCircuitHopRegistry (D9 step
 * 5c). MeshHost owns one when Amp is up; `AmpMediaRelayClient` adapts it to `IMediaRelayClient`
 * ([A020]).
 *
 * `local_server` (optional, must outlive this) is this node's `MediaRelayServer`: when the hop is
 * this node, `AttachAsLocalHop` joins the hosted session without dialing itself.
 */
class MediaRelayClientCoordinator {
public:
  using QuoteFinished = std::function<void(Roe<MediaRelayQuote>)>;
  using AttachFinished = std::function<void(Roe<MediaRelayAttachResult>)>;
  using FrameHandler = std::function<void(MediaDataFrame)>;

  explicit MediaRelayClientCoordinator(pp::amp::MeshRuntime& runtime, MediaRelayServer* local_server = nullptr);
  ~MediaRelayClientCoordinator();

  MediaRelayClientCoordinator(const MediaRelayClientCoordinator&) = delete;
  MediaRelayClientCoordinator& operator=(const MediaRelayClientCoordinator&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

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
  uint64_t AddClientTransportLostObserver(std::function<void(MediaRelayClientLoss)> observer);
  void RemoveClientTransportLostObserver(uint64_t token);
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
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
