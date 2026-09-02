#pragma once

#include "base/mesh/host/MeshPorts.h"
#include "base/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "base/mesh/l4/call_media/ICallMediaTransport.h"
#include "base/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/media/CallMediaHealth.h"

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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
  /**
   * Unexpected guest duplex death (not Detach). Default no-op for fakes that never lose transport.
   * Handler may be invoked on the mesh io thread.
   */
  virtual void SetClientTransportLostHandler(std::function<void()> /*handler*/) {}
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
  /** Hop health counters (V032); default empty. */
  virtual CallHopHealth HealthSnapshot() const { return {}; }
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
  /** Reach a call peer for 1:1 call-media when not directly dialable. */
  virtual Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) = 0;
};

/** Amp-only dial registry (PeerLinkManager + AmpCircuitHopRegistry). */
class PeerSessionDialRegistry final : public IDialRegistry {
public:
  PeerSessionDialRegistry() = default;

  void SetAmpLinks(IChatPeerLinks* amp_links) { amp_links_ = amp_links; }
  void SetAmpCircuitHops(AmpCircuitHopRegistry* hops) { amp_hops_ = hops; }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    if (amp_links_) {
      if (IsAdpMultiaddr(multiaddr)) {
        (void)amp_links_->RegisterEndpoint(peer_key, multiaddr);
        if (auto peer_id = PeerIdFromAdpMultiaddr(multiaddr); peer_id && *peer_id != peer_key) {
          (void)amp_links_->RegisterEndpoint(*peer_id, multiaddr);
        }
        return {};
      }
    }
    return Error("dial registry not available");
  }

  bool IsDialable(const std::string& peer_key) const override {
    if (amp_links_ && amp_links_->GetLinkSnapshot(peer_key).has_endpoint) {
      return true;
    }
    // SoftMigrate hop dialability is media-relay-specific. Call-media circuit hops
    // must not mark a peer dialable for quote/attach (TryEnsureCallMediaReachable
    // remains the call-media path and is protocol-keyed).
    return amp_hops_ && static_cast<bool>(amp_hops_->Find(peer_key, kMediaRelayProtocolId));
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (amp_links_) {
      if (auto amp_ma = amp_links_->PreferredMultiaddr(peer_key)) {
        return amp_ma;
      }
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& /*peer_key*/) override {}

  void AbortInflightDial(const std::string& /*peer_key*/) override {}

  void ClearCallMediaCircuitHop(const std::string& peer_key) override {
    if (amp_hops_) {
      amp_hops_->Clear(peer_key, kCallMediaDirectProtocolId);
    }
  }

private:
  IChatPeerLinks* amp_links_ = nullptr;
  AmpCircuitHopRegistry* amp_hops_ = nullptr;
};

/** Forwards to MessagingHub / CallStack wiring. */
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
