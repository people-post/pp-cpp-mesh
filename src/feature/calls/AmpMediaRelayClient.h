#pragma once

#include "domain/mesh/l4/media_relay/AmpMediaRelayCoordinator.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * IMediaRelayClient over AmpMediaRelayCoordinator ([A020]).
 * Prefer RequestQuoteAsync / AcceptAndAttachAsync when MeshPump + PostToIo are available;
 * sync wrappers park (do not Tick while MeshPump owns Drive).
 */
class AmpMediaRelayClient final : public IMediaRelayClient {
public:
  using IoPump = std::function<void()>;
  using IoPost = std::function<void(std::function<void()>)>;

  AmpMediaRelayClient(AmpMediaRelayCoordinator& coordinator, IoPump io_pump, std::string local_peer_id,
                      IoPost post_io = {});

  Roe<std::string> LocalPeerIdBase58() const override;
  bool IsStarted() const override;

  void RequestQuoteAsync(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                         std::function<void(Roe<MediaRelayQuote>)> on_done, int timeout_ms = 8000) override;
  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                    int timeout_ms = 8000) override;

  void AcceptAndAttachAsync(const std::string& hop_peer_key, const std::string& quote_id,
                            const std::string& call_id, const std::string& auth_stub,
                            std::function<void(MediaDataFrame)> on_frame,
                            std::function<void(Roe<MediaRelayAttachResult>)> on_done,
                            int timeout_ms = 8000) override;
  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& hop_peer_key, const std::string& quote_id,
                                              const std::string& call_id, const std::string& auth_stub,
                                              std::function<void(MediaDataFrame)> on_frame,
                                              int timeout_ms = 8000) override;

  void StartClientFrameReader() override;
  void SetClientTransportLostHandler(std::function<void()> handler) override;
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& call_id,
                                               std::function<void(MediaDataFrame)> on_frame) override;
  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) override;
  Roe<void> SendFrame(const MediaDataFrame& frame) override;
  void Detach() override;
  bool IsAttached() const override;
  bool IsLocalHopAttached() const override;
  double PathPressure() const override;
  CallHopHealth HealthSnapshot() const override;

private:
  AmpMediaRelayCoordinator& coordinator_;
  IoPump io_pump_;
  IoPost post_io_;
  std::string local_peer_id_;
};

} // namespace pbr
