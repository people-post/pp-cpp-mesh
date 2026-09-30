#include "domain/mesh/host/DisclosureGatedPeerLinks.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kNotDisclosed = "address not disclosed to this peer (privacy: direct connections)";

} // namespace

DisclosureGatedPeerLinks::DisclosureGatedPeerLinks(IChatPeerLinks& inner, const AddressDisclosureGate& gate)
    : inner_(inner), gate_(gate) {}

bool DisclosureGatedPeerLinks::MayDial(const std::string& peer_key) const {
  return gate_.AllowsDirect(peer_key) || inner_.IsConnected(peer_key);
}

std::optional<std::string> DisclosureGatedPeerLinks::PreferredMultiaddr(const std::string& peer_id) const {
  return inner_.PreferredMultiaddr(peer_id);
}

Roe<void> DisclosureGatedPeerLinks::RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  return inner_.RegisterEndpoint(peer_key, multiaddr);  // booking an address discloses nothing
}

Roe<void> DisclosureGatedPeerLinks::RegisterEndpoints(const std::string& peer_key,
                                                      const std::vector<std::string>& multiaddrs) {
  return inner_.RegisterEndpoints(peer_key, multiaddrs);
}

void DisclosureGatedPeerLinks::EnsureAssociation(const std::string& peer_key, LinkCb on_complete) {
  if (!MayDial(peer_key)) {
    if (on_complete) {
      on_complete(Failure::Of(Err::EndpointNotRegistered, kNotDisclosed));
    }
    return;
  }
  inner_.EnsureAssociation(peer_key, std::move(on_complete));
}

void DisclosureGatedPeerLinks::OpenChannel(const std::string& peer_key, const std::string& protocol_id,
                                           pp::amp::ChannelPolicy policy, ChannelCb on_complete) {
  if (!MayDial(peer_key)) {  // opening a channel dials first when no link is up
    if (on_complete) {
      on_complete(ChannelRoe(Failure::Of(Err::EndpointNotRegistered, kNotDisclosed)));
    }
    return;
  }
  inner_.OpenChannel(peer_key, protocol_id, std::move(policy), std::move(on_complete));
}

void DisclosureGatedPeerLinks::EstablishNestedOverCarrier(const std::string& peer_key,
                                                          std::shared_ptr<pp::amp::ChannelSession> carrier,
                                                          const bool initiator, LinkCb on_complete) {
  // Over a relay carrier the peer sees the relay, not us.
  inner_.EstablishNestedOverCarrier(peer_key, std::move(carrier), initiator, std::move(on_complete));
}

void DisclosureGatedPeerLinks::SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) {
  inner_.SetProtocolHandler(protocol_id, std::move(handler));
}

void DisclosureGatedPeerLinks::RemoveProtocolHandler(const std::string& protocol_id) {
  inner_.RemoveProtocolHandler(protocol_id);
}

MeshPeerLinkSnapshot DisclosureGatedPeerLinks::GetLinkSnapshot(const std::string& peer_key) const {
  MeshPeerLinkSnapshot snap = inner_.GetLinkSnapshot(peer_key);
  if (!MayDial(peer_key)) {
    snap.has_endpoint = false;  // there is an address, but not one we may dial
  }
  return snap;
}

pp::amp::LinkSnapshotEx DisclosureGatedPeerLinks::SnapshotByPeerId(const std::string& peer_id) const {
  return inner_.SnapshotByPeerId(peer_id);
}

bool DisclosureGatedPeerLinks::IsConnectedRelayed(const std::string& peer_id) const {
  return inner_.IsConnectedRelayed(peer_id);
}

bool DisclosureGatedPeerLinks::IsConnected(const std::string& peer_key) const {
  return inner_.IsConnected(peer_key);
}

bool DisclosureGatedPeerLinks::IsReachable(const std::string& peer_id) const {
  return MayDial(peer_id) ? inner_.IsReachable(peer_id) : false;
}

void DisclosureGatedPeerLinks::MarkWarm(const std::string& peer_key) {
  inner_.MarkWarm(peer_key);
}

void DisclosureGatedPeerLinks::ClearDialBackoff(const std::string& peer_key) {
  inner_.ClearDialBackoff(peer_key);
}

void DisclosureGatedPeerLinks::AbortInflightDial(const std::string& peer_key) {
  inner_.AbortInflightDial(peer_key);
}

void DisclosureGatedPeerLinks::DropLink(const std::string& peer_key) {
  inner_.DropLink(peer_key);
}

void DisclosureGatedPeerLinks::WhenChannelOpen(const std::string& peer_key, const uint32_t channel_id,
                                               const int64_t deadline_ms, std::function<void(bool ok)> done) {
  inner_.WhenChannelOpen(peer_key, channel_id, deadline_ms, std::move(done));
}

std::shared_ptr<pp::amp::ChannelSession> DisclosureGatedPeerLinks::BindChannel(
    const std::string& peer_key, const uint32_t channel_id, pp::amp::ChannelPolicy policy,
    pp::amp::ChannelSession::FrameHandler on_frame, pp::amp::ChannelSession::ClosedCallback on_closed) {
  return inner_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
}

} // namespace pbr
