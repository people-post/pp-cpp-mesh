#include "base/mesh/host/MeshPorts.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/link/Types.h"

namespace pbr {

namespace {

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
  const auto& amp_failure = result.error();
  return IChatPeerLinks::LinkRoe::error(IChatPeerLinks::Failure::Of(
      static_cast<IChatPeerLinks::Err>(static_cast<int32_t>(amp_failure.GetCode())), amp_failure.message));
}

IChatPeerLinks::ChannelRoe ToChannelRoe(const pp::amp::PeerLinkManager::ChannelRoe& result) {
  if (result) {
    return *result;
  }
  const auto& amp_failure = result.error();
  return IChatPeerLinks::ChannelRoe::error(IChatPeerLinks::Failure::Of(
      static_cast<IChatPeerLinks::Err>(static_cast<int32_t>(amp_failure.GetCode())), amp_failure.message));
}

} // namespace

MeshPeerLinkSnapshot ToMeshPeerLinkSnapshot(const pp::amp::PeerLinkSnapshot& snap) {
  MeshPeerLinkSnapshot out;
  out.phase = ToPhase(snap.phase);
  out.backoff_remaining = snap.backoff_remaining;
  out.detail = snap.detail;
  out.has_endpoint = snap.has_endpoint;
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

class AmpChatPeerLinks final : public IChatPeerLinks {
public:
  explicit AmpChatPeerLinks(pp::amp::PeerLinkManager& links) : links_(links) {}

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const override {
    return links_.PreferredMultiaddr(peer_id);
  }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    return links_.RegisterEndpoint(peer_key, multiaddr);
  }

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) override {
    links_.EnsureAssociation(peer_key, [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::LinkRoe result) {
      on_complete(ToLinkRoe(result));
    });
  }

  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, pp::amp::ChannelPolicy policy,
                   ChannelCb on_complete) override {
    links_.OpenChannel(peer_key, protocol_id, std::move(policy),
                       [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::ChannelRoe result) {
                         on_complete(ToChannelRoe(result));
                       });
  }

  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<pp::amp::ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete) override {
    links_.EstablishNestedOverCarrier(
        peer_key, std::move(carrier), initiator,
        [on_complete = std::move(on_complete)](pp::amp::PeerLinkManager::LinkRoe result) {
          on_complete(ToLinkRoe(result));
        });
  }

  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) override {
    links_.SetProtocolHandler(protocol_id, std::move(handler));
  }

  void RemoveProtocolHandler(const std::string& protocol_id) override {
    links_.RemoveProtocolHandler(protocol_id);
  }

  MeshPeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const override {
    return ToMeshPeerLinkSnapshot(links_.GetLinkSnapshot(peer_key));
  }

  bool IsConnected(const std::string& peer_key) const override { return links_.IsConnected(peer_key); }

  void MarkWarm(const std::string& peer_key) override { links_.MarkWarm(peer_key); }

  pp::amp::PeerLink* FindLink(const std::string& peer_key) override { return links_.FindLink(peer_key); }

  const pp::amp::PeerLink* FindLink(const std::string& peer_key) const override { return links_.FindLink(peer_key); }

private:
  pp::amp::PeerLinkManager& links_;
};

std::unique_ptr<IChatPeerLinks> NewAmpChatPeerLinks(pp::amp::PeerLinkManager& links) {
  return std::make_unique<AmpChatPeerLinks>(links);
}

} // namespace pbr
