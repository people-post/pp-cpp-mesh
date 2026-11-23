#include "mesh/host/MeshPorts.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/link/Types.h"
#include "common/Logger.h"
#include "mesh/host/AmpLinkErr.h"
#include "mesh/shared/AmpChannelOpen.h"

#include <chrono>

namespace pp::mesh {

namespace {

logging::Logger& MeshPortsLog() {
  static logging::Logger log = logging::getLogger("MeshPorts");
  return log;
}

MeshPeerLinkPhase ToPhase(pp::amp::PeerLinkPhase phase) {
  switch (phase) {
    case pp::amp::PeerLinkPhase::Unavailable:
      return MeshPeerLinkPhase::Unavailable;
    case pp::amp::PeerLinkPhase::Idle:
      return MeshPeerLinkPhase::Idle;
    case pp::amp::PeerLinkPhase::Dialing:
      return MeshPeerLinkPhase::Dialing;
    case pp::amp::PeerLinkPhase::Handshaking:
      return MeshPeerLinkPhase::Handshaking;
    case pp::amp::PeerLinkPhase::Connected:
      return MeshPeerLinkPhase::Connected;
    case pp::amp::PeerLinkPhase::Backoff:
      return MeshPeerLinkPhase::Backoff;
  }
  return MeshPeerLinkPhase::Unavailable;
}

IChatPeerLinks::LinkRoe ToLinkRoe(const pp::amp::PeerLinkManager::LinkRoe& result) {
  if (result) {
    return {};
  }
  return IChatPeerLinks::LinkRoe::error(ToMeshLinkFailure(result.error()));
}

IChatPeerLinks::ChannelRoe ToChannelRoe(const pp::amp::PeerLinkManager::ChannelRoe& result) {
  if (result) {
    return *result;
  }
  return IChatPeerLinks::ChannelRoe::error(ToMeshLinkFailure(result.error()));
}

// IChatPeerLinks::Err is Amp's PeerLinkManager::Err, code for code (MeshPorts.h keeps Amp's link headers out of
// feature code, so it is its own enum): each value is Amp's, and Generic is last in both — so neither has a code
// the other lacks between them. ToMeshLinkErr below names every Amp code, so one Amp adds fails the build there.
#define PP_MESH_SAME_LINK_ERR(name)                                                                                 \
  static_assert(                                                                                                    \
      static_cast<int32_t>(IChatPeerLinks::Err::name) == static_cast<int32_t>(pp::amp::PeerLinkManager::Err::name), \
      "IChatPeerLinks::Err::" #name " is not Amp's PeerLinkManager::Err::" #name)
PP_MESH_SAME_LINK_ERR(Ok);
PP_MESH_SAME_LINK_ERR(EndpointNotRegistered);
PP_MESH_SAME_LINK_ERR(DialInBackoff);
PP_MESH_SAME_LINK_ERR(TooManyConcurrentDials);
PP_MESH_SAME_LINK_ERR(MaxLinksReached);
PP_MESH_SAME_LINK_ERR(AssociationNotReady);
PP_MESH_SAME_LINK_ERR(LinkNotFound);
PP_MESH_SAME_LINK_ERR(NestedCarrierIncomplete);
PP_MESH_SAME_LINK_ERR(DialTimeout);
PP_MESH_SAME_LINK_ERR(HandshakeFailed);
PP_MESH_SAME_LINK_ERR(TransportFailed);
PP_MESH_SAME_LINK_ERR(DualDialLost);
PP_MESH_SAME_LINK_ERR(ChannelOpenFailed);
PP_MESH_SAME_LINK_ERR(VersionMismatch);
PP_MESH_SAME_LINK_ERR(Generic);
#undef PP_MESH_SAME_LINK_ERR

} // namespace

// No `default:` — and a missing case is an error, not a warning, on every compiler — so a code Amp adds cannot
// fall through to Generic unnoticed (the way VersionMismatch once did).
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(1 : 4062) // off by default: on, and an error
#pragma warning(error : 4062)
#endif
IChatPeerLinks::Err ToMeshLinkErr(const pp::amp::PeerLinkManager::Err code) {
  using Amp = pp::amp::PeerLinkManager::Err;
  using Mesh = IChatPeerLinks::Err;
  switch (code) {
    case Amp::Ok:
      return Mesh::Ok;
    case Amp::EndpointNotRegistered:
      return Mesh::EndpointNotRegistered;
    case Amp::DialInBackoff:
      return Mesh::DialInBackoff;
    case Amp::TooManyConcurrentDials:
      return Mesh::TooManyConcurrentDials;
    case Amp::MaxLinksReached:
      return Mesh::MaxLinksReached;
    case Amp::AssociationNotReady:
      return Mesh::AssociationNotReady;
    case Amp::LinkNotFound:
      return Mesh::LinkNotFound;
    case Amp::NestedCarrierIncomplete:
      return Mesh::NestedCarrierIncomplete;
    case Amp::DialTimeout:
      return Mesh::DialTimeout;
    case Amp::HandshakeFailed:
      return Mesh::HandshakeFailed;
    case Amp::TransportFailed:
      return Mesh::TransportFailed;
    case Amp::DualDialLost:
      return Mesh::DualDialLost;
    case Amp::ChannelOpenFailed:
      return Mesh::ChannelOpenFailed;
    case Amp::VersionMismatch:
      return Mesh::VersionMismatch;
    case Amp::Generic:
      return Mesh::Generic;
  }
  return Mesh::Generic; // not an Amp enumerator
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

IChatPeerLinks::Failure ToMeshLinkFailure(const pp::amp::PeerLinkManager::Failure& failure) {
  return IChatPeerLinks::Failure::Of(ToMeshLinkErr(failure.GetCode()), failure.message);
}

MeshPeerLinkSnapshot ToMeshPeerLinkSnapshot(const pp::amp::PeerLinkSnapshot& snap) {
  MeshPeerLinkSnapshot out;
  out.phase = ToPhase(snap.phase);
  out.backoff_remaining = snap.backoff_remaining;
  out.detail = snap.detail;
  out.has_endpoint = snap.has_endpoint;
  out.carrier_backed = snap.carrier_backed;
  out.multiaddr = snap.multiaddr;
  return out;
}

bool IsAdpMultiaddr(const std::string& multiaddr) {
  return static_cast<bool>(pp::amp::ParseAdpMultiaddr(multiaddr));
}

std::optional<std::string> PeerIdFromAdpMultiaddr(const std::string& multiaddr) {
  if (auto parsed = pp::amp::ParseAdpMultiaddr(multiaddr)) {
    if (!parsed->peer_id.empty()) {
      return parsed->peer_id;
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> UdpPortFromAdpMultiaddr(const std::string& multiaddr) {
  if (auto parsed = pp::amp::ParseAdpMultiaddr(multiaddr)) {
    return parsed->endpoint.port;
  }
  return std::nullopt;
}

/**
 * PeerLinkManager is mutated under MeshRuntime::io_mu_ (Drive / PostToIo).
 * Lock the same mutex for every façade call so Coordinator/UI reads cannot race
 * FinishDial / ScheduleDropLink (dogfood 091029 / 091740).
 */
class AmpChatPeerLinks final : public IChatPeerLinks {
public:
  explicit AmpChatPeerLinks(pp::amp::MeshRuntime& runtime) : runtime_(runtime), links_(runtime.Links()) {}

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const override {
    return runtime_.WithIoLock([&] { return links_.PreferredMultiaddr(peer_id); });
  }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    return runtime_.WithIoLock([&] { return links_.RegisterEndpoint(peer_key, multiaddr); });
  }

  Roe<void> RegisterEndpoints(const std::string& peer_key, const std::vector<std::string>& multiaddrs) override {
    return runtime_.WithIoLock([&] { return links_.RegisterEndpoints(peer_key, multiaddrs); });
  }

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) override {
    runtime_.WithIoLock([&] {
      links_.EnsureAssociation(peer_key,
                               [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::LinkRoe result) {
                                 on_complete(ToLinkRoe(result));
                               });
    });
  }

  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, pp::amp::ChannelPolicy policy,
                   ChannelCb on_complete) override {
    runtime_.WithIoLock([&] {
      links_.OpenChannel(peer_key, protocol_id, std::move(policy),
                         [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::ChannelRoe result) {
                           on_complete(ToChannelRoe(result));
                         });
    });
  }

  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<pp::amp::ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete) override {
    runtime_.WithIoLock([&] {
      links_.EstablishNestedOverCarrier(
          peer_key, std::move(carrier), initiator,
          [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::LinkRoe result) {
            on_complete(ToLinkRoe(result));
          });
    });
  }

  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) override {
    runtime_.WithIoLock([&] { links_.SetProtocolHandler(protocol_id, std::move(handler)); });
  }

  void RemoveProtocolHandler(const std::string& protocol_id) override {
    runtime_.WithIoLock([&] { links_.RemoveProtocolHandler(protocol_id); });
  }

  MeshPeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const override {
    return runtime_.WithIoLock([&] { return ToMeshPeerLinkSnapshot(links_.GetLinkSnapshot(peer_key)); });
  }

  pp::amp::LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id) const override {
    return runtime_.WithIoLock([&] { return links_.GetSnapshotByPeerId(peer_id); });
  }

  bool IsConnected(const std::string& peer_key) const override {
    return runtime_.WithIoLock([&] { return links_.IsConnected(peer_key); });
  }

  bool IsConnectedRelayed(const std::string& peer_id) const override {
    return runtime_.WithIoLock([&] {
      const auto snap = links_.GetSnapshotByPeerId(peer_id, pp::amp::TransportClass::Carrier);
      return snap.transport == pp::amp::TransportClass::Carrier && snap.base.phase == pp::amp::PeerLinkPhase::Connected;
    });
  }

  bool IsReachable(const std::string& peer_id) const override {
    return runtime_.WithIoLock([&] { return links_.IsReachable(peer_id); });
  }

  void MarkWarm(const std::string& peer_key) override {
    runtime_.WithIoLock([&] { links_.MarkWarm(peer_key); });
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    runtime_.WithIoLock([&] { links_.ClearDialBackoff(peer_key); });
  }

  void AbortInflightDial(const std::string& peer_key) override {
    runtime_.WithIoLock([&] { links_.AbortInflightDial(peer_key); });
  }

  void VerifyLink(const std::string& peer_key, const std::chrono::milliseconds heard_within,
                  std::function<void(bool alive)> done) override {
    runtime_.WithIoLock([&] { links_.VerifyLink(peer_key, heard_within, std::move(done)); });
  }

  void DropLink(const std::string& peer_key) override {
    // Amp schedules the drop on Tick (reason "requested" in the MeshLink log); both ends evict.
    const size_t n = runtime_.RequestDropLink(peer_key);
    if (n > 0) {
      MeshPortsLog().info << "amp link drop requested peer_key=" << peer_key << " links=" << n;
    }
  }

  void WhenChannelOpen(const std::string& peer_key, uint32_t channel_id, int64_t deadline_ms,
                       std::function<void(bool ok)> done) override {
    // IChatPeerLinks contract: steady_clock epoch ms. Amp WhenChannelOpen wants Amp clock.
    runtime_.WithIoLock([&] {
      const auto steady_deadline = std::chrono::steady_clock::time_point(std::chrono::milliseconds(deadline_ms));
      AmpWhenChannelOpen(links_, peer_key, channel_id, steady_deadline, std::move(done));
    });
  }

  std::shared_ptr<pp::amp::ChannelSession> BindChannel(const std::string& peer_key, uint32_t channel_id,
                                                       pp::amp::ChannelPolicy policy,
                                                       pp::amp::ChannelSession::FrameHandler on_frame,
                                                       pp::amp::ChannelSession::ClosedCallback on_closed) override {
    return runtime_.WithIoLock([&] {
      return links_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
    });
  }

  std::shared_ptr<pp::amp::ChannelSession> BindChannelOnLink(
      pp::amp::LinkHandle link, uint32_t channel_id, pp::amp::ChannelPolicy policy,
      pp::amp::ChannelSession::FrameHandler on_frame, pp::amp::ChannelSession::ClosedCallback on_closed) override {
    return runtime_.WithIoLock([&] {
      return links_.BindChannelOnLink(link, channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
    });
  }

private:
  pp::amp::MeshRuntime& runtime_;
  pp::amp::PeerLinkManager& links_;
};

std::unique_ptr<IChatPeerLinks> NewAmpChatPeerLinks(pp::amp::MeshRuntime& runtime) {
  return std::make_unique<AmpChatPeerLinks>(runtime);
}

} // namespace pp::mesh
