#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/LinkIdentity.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp::amp {
class MeshRuntime;
struct PeerLinkSnapshot;
} // namespace pp::amp

namespace pbr {

/** Mesh-layer aliases for link UX (avoids feature → amp/link/Types.h). */
enum class MeshPeerLinkPhase {
  Unavailable,
  Idle,
  Dialing,
  Handshaking,
  Connected,
  Backoff,
};

struct MeshPeerLinkSnapshot {
  MeshPeerLinkPhase phase = MeshPeerLinkPhase::Unavailable;
  std::chrono::milliseconds backoff_remaining{0};
  std::string detail;
  bool has_endpoint = false;
  bool carrier_backed = false;
  std::string multiaddr;
};

MeshPeerLinkSnapshot ToMeshPeerLinkSnapshot(const pp::amp::PeerLinkSnapshot& snap);

using MeshWorkerPost = std::function<void(std::function<void()>)>;

struct MeshIoContext {
  std::function<void()> io_pump;
  MeshWorkerPost post_worker;
  /** MeshRuntime::PostToIo for A022-style channel-open polls (optional). */
  MeshWorkerPost post_io;
  std::string local_peer_id;
  std::string listen_multiaddr;
};

/**
 * Narrow peer-link port for feature chat/history/blob/circuit reach.
 * Implemented in mesh/host by wrapping Amp PeerLinkManager (sole amp/link consumer).
 *
 * Affinity: PeerLinkManager mutations run under MeshRuntime::io_mu_ (Drive/PostToIo).
 * AmpChatPeerLinks takes the same lock via WithIoLock for every call — Coordinator/UI
 * may read IsConnected safely. Prefer post_io for multi-step dial/circuit work.
 * See THREADING.md § Thread affinity.
 *
 * Prefer BindChannel / WhenChannelOpen / SnapshotByPeerId / IsReachable over raw PeerLink*.
 */
class IChatPeerLinks {
public:
  enum class Err : int32_t {
    Ok = 0,
    EndpointNotRegistered,
    DialInBackoff,
    TooManyConcurrentDials,
    MaxLinksReached,
    AssociationNotReady,
    LinkNotFound,
    NestedCarrierIncomplete,
    DialTimeout,
    HandshakeFailed,
    TransportFailed,
    DualDialLost,
    ChannelOpenFailed,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using LinkRoe = CodedRoe<void, Err>;
  using ChannelRoe = CodedRoe<uint32_t, Err>;
  using LinkCb = std::function<void(LinkRoe)>;
  using ChannelCb = std::function<void(ChannelRoe)>;
  using ProtocolHandler =
      std::function<void(pp::amp::LinkHandle handle, const std::string& remote_peer_id, uint32_t channel_id)>;

  /** Stable codes mirror `PeerLinkManager::Err` — see docs/contracts/AMP-LINK-ERRORS.md. */
  static bool IsAssociationNotReady(const Failure& failure) {
    return failure.GetCode() == Err::AssociationNotReady;
  }

  static bool IsDialInBackoff(const Failure& failure) { return failure.GetCode() == Err::DialInBackoff; }

  static bool IsEndpointNotRegistered(const Failure& failure) {
    return failure.GetCode() == Err::EndpointNotRegistered;
  }

  virtual ~IChatPeerLinks() = default;

  virtual std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const = 0;
  virtual Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) = 0;

  virtual void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) = 0;
  virtual void OpenChannel(const std::string& peer_key, const std::string& protocol_id, pp::amp::ChannelPolicy policy,
                           ChannelCb on_complete) = 0;
  virtual void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<pp::amp::ChannelSession> carrier,
                                          bool initiator, LinkCb on_complete) = 0;

  virtual void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) = 0;
  virtual void RemoveProtocolHandler(const std::string& protocol_id) = 0;

  virtual MeshPeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const = 0;
  virtual pp::amp::LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id) const = 0;
  virtual bool IsConnected(const std::string& peer_key) const = 0;
  virtual bool IsReachable(const std::string& peer_id) const = 0;

  virtual void MarkWarm(const std::string& peer_key) = 0;

  /** Clear PeerLinkManager dial-failure cooldown (default no-op for fakes). */
  virtual void ClearDialBackoff(const std::string& /*peer_key*/) {}
  /** Abort in-flight EnsureAssociation without arming a new backoff (default no-op). */
  virtual void AbortInflightDial(const std::string& /*peer_key*/) {}

  /**
   * Poll until mux channel is open or `deadline_ms` (steady_clock epoch ms).
   * AmpChatPeerLinks converts to Amp clock before PeerLinkManager::WhenChannelOpen.
   */
  virtual void WhenChannelOpen(const std::string& peer_key, uint32_t channel_id, int64_t deadline_ms,
                               std::function<void(bool ok)> done) = 0;
  /**
   * Bind ChannelSession under strand lock; empty if link/mux missing.
   */
  virtual std::shared_ptr<pp::amp::ChannelSession> BindChannel(
      const std::string& peer_key, uint32_t channel_id, pp::amp::ChannelPolicy policy,
      pp::amp::ChannelSession::FrameHandler on_frame,
      pp::amp::ChannelSession::ClosedCallback on_closed = {}) = 0;
};

struct MeshChatDeps {
  MeshIoContext io;
  IChatPeerLinks& links;
};

struct MeshCircuitDeps {
  CircuitTunnelCoordinator& tunnel;
  AmpCircuitHopRegistry& hops;
  IChatPeerLinks& links;
};

struct MeshCallMediaDeps {
  ICallMediaTransport& transport;
};

/** Returns true when `multiaddr` is a parseable ADP multiaddr. */
bool IsAdpMultiaddr(const std::string& multiaddr);

/** Extract PeerId from ADP multiaddr when present. */
std::optional<std::string> PeerIdFromAdpMultiaddr(const std::string& multiaddr);

/** UDP port from ADP multiaddr when parseable. */
std::optional<uint16_t> UdpPortFromAdpMultiaddr(const std::string& multiaddr);

/** Wrap Amp MeshRuntime Links() under WithIoLock for feature-layer chat/circuit ports. */
std::unique_ptr<IChatPeerLinks> NewAmpChatPeerLinks(pp::amp::MeshRuntime& runtime);

} // namespace pbr
