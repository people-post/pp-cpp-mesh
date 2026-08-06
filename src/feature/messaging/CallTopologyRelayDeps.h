#pragma once

#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/CallMediaDirectService.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Narrow client surface for CallTopologyController (fakeable in unit tests). */
class IMediaRelayClient {
public:
  virtual ~IMediaRelayClient() = default;

  virtual Roe<std::string> LocalPeerIdBase58() const = 0;
  virtual bool IsStarted() const = 0;
  virtual Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            int timeout_ms = 8000) = 0;
  virtual Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame,
      int timeout_ms = 8000) = 0;
  /** After AcceptAndAttach + StartSfu — begin inbound frame delivery. */
  virtual void StartClientFrameReader() = 0;
  /** In-call hop: join local HostSession without dialing self. */
  virtual Roe<MediaRelayAttachResult> AttachAsLocalHop(
      const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) = 0;
  virtual Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) = 0;
  virtual Roe<void> SendFrame(const MediaDataFrame& frame) = 0;
  virtual void Detach() = 0;
  virtual bool IsAttached() const = 0;
  virtual bool IsLocalHopAttached() const = 0;
  /** Hop drop pressure 0..1 (V032); default 0 for fakes. */
  virtual double PathPressure() const { return 0.0; }
};

/** Dial registry surface for hop RegisterEndpoint / IsDialable. */
class IDialRegistry {
public:
  virtual ~IDialRegistry() = default;

  virtual Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) = 0;
  virtual bool IsDialable(const std::string& peer_key) const = 0;
  virtual std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const = 0;
  virtual void ClearDialBackoff(const std::string& peer_key) = 0;
  virtual void AbortInflightDial(const std::string& peer_key) = 0;
  virtual void ClearCallMediaCircuitHop(const std::string& peer_key) = 0;
};

/** L3: circuit bridge fallback when hop PeerId is not directly dialable. */
class ICircuitHopReach {
public:
  virtual ~ICircuitHopReach() = default;
  /** Reach a media_relay hop (topology / prefetch). */
  virtual Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) = 0;
  /** Reach a call peer for 1:1 libp2p call-media when not directly dialable. */
  virtual Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) = 0;
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

  bool IsStarted() const override { return service_ && service_->IsStarted(); }

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

  void StartClientFrameReader() override {
    if (service_) {
      service_->StartClientFrameReader();
    }
  }

  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& call_id,
                                               std::function<void(MediaDataFrame)> on_frame) override {
    if (!service_) {
      return Error("media_relay not available");
    }
    return service_->AttachAsLocalHop(call_id, std::move(on_frame));
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

  bool IsLocalHopAttached() const override {
    return service_ && service_->IsLocalHopAttached();
  }

  double PathPressure() const override {
    return service_ ? service_->PathPressure() : 0.0;
  }

private:
  MediaRelayService* service_ = nullptr;
};

/** Forwards to PeerSessionManager. */
class PeerSessionDialRegistry final : public IDialRegistry {
public:
  explicit PeerSessionDialRegistry(PeerSessionManager* sessions) : sessions_(sessions) {}

  void SetSessions(PeerSessionManager* sessions) { sessions_ = sessions; }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    if (!sessions_) {
      return Error("dial registry not available");
    }
    auto registered = sessions_->RegisterEndpoint(peer_key, multiaddr);
    if (!registered) {
      return registered;
    }
    // Pin hop in the address book above mDNS — OpenStream hydrates Preferred from the book
    // and would otherwise dial a poisoned virbr multiaddr over CallSfuAttach's LAN hop.
    std::string peer_id = peer_key;
    const auto p2p_pos = multiaddr.rfind("/p2p/");
    if (p2p_pos != std::string::npos) {
      peer_id = multiaddr.substr(p2p_pos + 5);
      const auto slash = peer_id.find('/');
      if (slash != std::string::npos) {
        peer_id.resize(slash);
      }
    }
    if (!peer_id.empty()) {
      (void)sessions_->UpsertBookEntry(peer_id, multiaddr, PeerAddrSource::CallHop);
    }
    return {};
  }

  bool IsDialable(const std::string& peer_key) const override {
    return sessions_ && sessions_->IsDialable(peer_key);
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (!sessions_) {
      return std::nullopt;
    }
    return sessions_->PreferredPeerMultiaddr(peer_key);
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    if (sessions_) {
      sessions_->ClearDialBackoff(peer_key);
    }
  }

  void AbortInflightDial(const std::string& peer_key) override {
    if (sessions_) {
      sessions_->AbortInflightDial(peer_key);
    }
  }

  void ClearCallMediaCircuitHop(const std::string& peer_key) override {
    if (sessions_) {
      sessions_->ClearCircuitHop(peer_key, kCallMediaDirectProtocolId);
    }
  }

private:
  PeerSessionManager* sessions_ = nullptr;
};

/** Forwards to PeerSessionManager + CircuitRelayService via MessagingHub wiring. */
class CircuitHopReachClient final : public ICircuitHopReach {
public:
  CircuitHopReachClient(std::function<Roe<void>(const std::string&)> try_media_hop_reach,
                        std::function<Roe<void>(const std::string&)> try_call_media_reach)
      : try_media_hop_reach_(std::move(try_media_hop_reach)),
        try_call_media_reach_(std::move(try_call_media_reach)) {}

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override {
    if (!try_media_hop_reach_) {
      return Error("circuit reach not available");
    }
    return try_media_hop_reach_(hop_peer_id);
  }

  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override {
    if (!try_call_media_reach_) {
      return Error("call-media circuit reach not available");
    }
    return try_call_media_reach_(peer_key);
  }

private:
  std::function<Roe<void>(const std::string&)> try_media_hop_reach_;
  std::function<Roe<void>(const std::string&)> try_call_media_reach_;
};

} // namespace pbr
