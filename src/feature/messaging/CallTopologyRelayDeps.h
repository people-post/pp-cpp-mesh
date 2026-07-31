#pragma once

#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include "common/Error.h"

#include <functional>
#include <string>

namespace pbr {

/** Narrow client surface for CallTopologyController (fakeable in unit tests). */
class IMediaRelayClient {
public:
  virtual ~IMediaRelayClient() = default;

  virtual Roe<std::string> LocalPeerIdBase58() const = 0;
  virtual Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            int timeout_ms = 8000) = 0;
  virtual Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame,
      int timeout_ms = 8000) = 0;
  virtual Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) = 0;
  virtual Roe<void> SendFrame(const MediaDataFrame& frame) = 0;
  virtual void Detach() = 0;
  virtual bool IsAttached() const = 0;
};

/** Dial registry surface for hop RegisterEndpoint / IsDialable. */
class IDialRegistry {
public:
  virtual ~IDialRegistry() = default;

  virtual Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) = 0;
  virtual bool IsDialable(const std::string& peer_key) const = 0;
  virtual void ClearDialBackoff(const std::string& peer_key) = 0;
};

/** Forwards to MediaRelayService. */
class MediaRelayServiceClient final : public IMediaRelayClient {
public:
  explicit MediaRelayServiceClient(MediaRelayService* service) : service_(service) {}

  Roe<std::string> LocalPeerIdBase58() const override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->LocalPeerIdBase58();
  }

  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                    const MediaRelayQuoteRequest& request,
                                    int timeout_ms) override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->RequestQuote(hop_peer_key, request, timeout_ms);
  }

  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& hop_peer_key,
                                              const std::string& quote_id,
                                              const std::string& call_id,
                                              const std::string& auth_stub,
                                              std::function<void(MediaDataFrame)> on_frame,
                                              int timeout_ms) override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->AcceptAndAttach(hop_peer_key, quote_id, call_id, auth_stub, std::move(on_frame),
                                     timeout_ms);
  }

  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->Subscribe(stream_id, channel_id);
  }

  Roe<void> SendFrame(const MediaDataFrame& frame) override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->SendFrame(frame);
  }

  void Detach() override {
    if (service_) {
      service_->Detach();
    }
  }

  bool IsAttached() const override {
    return service_ && service_->IsAttached();
  }

private:
  MediaRelayService* service_ = nullptr;
};

/** Forwards to PeerSessionManager. */
class PeerSessionDialRegistry final : public IDialRegistry {
public:
  explicit PeerSessionDialRegistry(PeerSessionManager* sessions) : sessions_(sessions) {}

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    if (!sessions_) {
      return Error("dial registry not available");
    }
    return sessions_->RegisterEndpoint(peer_key, multiaddr);
  }

  bool IsDialable(const std::string& peer_key) const override {
    return sessions_ && sessions_->IsDialable(peer_key);
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    if (sessions_) {
      sessions_->ClearDialBackoff(peer_key);
    }
  }

private:
  PeerSessionManager* sessions_ = nullptr;
};

} // namespace pbr
