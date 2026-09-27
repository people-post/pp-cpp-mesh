#pragma once

#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/media/CallMediaHealth.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Client surface of the `media_relay` L4 protocol (quote, attach, subscribe, send). Feature-
 * neutral: the session id and auth are opaque (a call id today; a broadcast session later).
 * Fakeable in unit tests. Wire field names are unchanged — relays parse the JSON field
 * `call_id`.
 */
/** Why a client's media_relay session ended without its owner's own Detach (observers). */
enum class MediaRelayClientLoss : uint8_t {
  /** The channel to the hop died. */
  TransportLost,
  /** Another attach took the (single) client session. */
  Replaced,
  /** Someone detached the client session. */
  Detached,
};

class IMediaRelayClient {
public:
  virtual ~IMediaRelayClient() = default;

  virtual Roe<std::string> LocalPeerIdBase58() const = 0;
  virtual bool IsStarted() const = 0;
  virtual Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            int timeout_ms = 8000) = 0;
  /** Prefer over sync RequestQuote when MeshPump + PostToIo are available. */
  virtual void RequestQuoteAsync(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                 std::function<void(Roe<MediaRelayQuote>)> on_done, int timeout_ms = 8000) {
    if (on_done) {
      on_done(RequestQuote(hop_peer_key, request, timeout_ms));
    }
  }
  virtual Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& session_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame,
      int timeout_ms = 8000) = 0;
  virtual void AcceptAndAttachAsync(const std::string& hop_peer_key, const std::string& quote_id,
                                    const std::string& session_id, const std::string& auth_stub,
                                    std::function<void(MediaDataFrame)> on_frame,
                                    std::function<void(Roe<MediaRelayAttachResult>)> on_done,
                                    int timeout_ms = 8000) {
    if (on_done) {
      on_done(AcceptAndAttach(hop_peer_key, quote_id, session_id, auth_stub, std::move(on_frame), timeout_ms));
    }
  }
  /** After AcceptAndAttach + StartSfu — begin inbound frame delivery. */
  virtual void StartClientFrameReader() = 0;
  /**
   * Client session-end observers (each feature that attaches registers its own): transport loss,
   * replacement by another attach, and Detach. Every observer hears every end; each decides whether
   * the session was its own (e.g. remove the observer before your own Detach, or react to
   * TransportLost only). Returns a token for Remove (0 = unsupported). Delivered on the mesh io
   * thread, never under the client's lock; a notice already queued may still arrive after Remove.
   */
  virtual uint64_t AddClientTransportLostObserver(std::function<void(MediaRelayClientLoss)> /*observer*/) {
    return 0;
  }
  virtual void RemoveClientTransportLostObserver(uint64_t /*token*/) {}
  /** In-call hop: join local HostSession without dialing self. */
  virtual Roe<MediaRelayAttachResult> AttachAsLocalHop(
      const std::string& session_id, std::function<void(MediaDataFrame)> on_frame) = 0;
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

} // namespace pbr
