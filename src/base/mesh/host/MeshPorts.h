#pragma once

#include "amp/L3/ChannelPolicy.h"
#include "base/mesh/l4/call_media/ICallMediaTransport.h"
#include "base/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "base/mesh/l4/circuit/CircuitTunnelCoordinator.h"
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
class ChannelSession;
class PeerLink;
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
  std::string multiaddr;
};

MeshPeerLinkSnapshot ToMeshPeerLinkSnapshot(const pp::amp::PeerLinkSnapshot& snap);

using MeshWorkerPost = std::function<void(std::function<void()>)>;

struct MeshIoContext {
  std::function<void()> io_pump;
  MeshWorkerPost post_worker;
  std::string local_peer_id;
  std::string listen_multiaddr;
};

/**
 * Narrow peer-link port for feature chat/history/blob/circuit reach.
 * Implemented in mesh/host by wrapping Amp PeerLinkManager (sole amp/link consumer).
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

  using LinkRoe = Roe<void>;
  using ChannelRoe = Roe<uint32_t>;
  using LinkCb = std::function<void(LinkRoe)>;
  using ChannelCb = std::function<void(ChannelRoe)>;
  using ProtocolHandler = std::function<void(pp::amp::PeerLink& link, uint32_t channel_id)>;

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
  virtual bool IsConnected(const std::string& peer_key) const = 0;

  virtual void MarkWarm(const std::string& peer_key) = 0;

  virtual pp::amp::PeerLink* FindLink(const std::string& peer_key) = 0;
  virtual const pp::amp::PeerLink* FindLink(const std::string& peer_key) const = 0;
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

/** Wrap Amp PeerLinkManager for feature-layer chat/circuit ports. */
std::unique_ptr<IChatPeerLinks> NewAmpChatPeerLinks(pp::amp::PeerLinkManager& links);

} // namespace pbr
