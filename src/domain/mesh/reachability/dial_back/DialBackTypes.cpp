#include "domain/mesh/reachability/dial_back/DialBackTypes.h"

#include "common/net/PublicAddress.h"

#include <cstring>

namespace pbr {

DialBackFailure WrapDialBackLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
  using Err = DialBackErr;
  using Failure = DialBackFailure;
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("dial-back: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("dial-back: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("dial-back: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("dial-back: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("dial-back: link error", "link", child.message));
  }
}

/** Public-routable (not private / loopback / link-local / CGNAT / multicast / test) address. */
static bool IsPublicEndpoint(const pp::adp::IpEndpoint& ep) {
  if (ep.family == pp::adp::IpEndpoint::Family::V4) {
    const uint32_t host_order = (static_cast<uint32_t>(ep.addr[0]) << 24) | (static_cast<uint32_t>(ep.addr[1]) << 16) |
                                (static_cast<uint32_t>(ep.addr[2]) << 8) | static_cast<uint32_t>(ep.addr[3]);
    return IsPublicIPv4(host_order);
  }
  return IsPublicIPv6(ep.addr.data());
}

bool DialBackTargetAllowed(const pp::adp::IpEndpoint& target, const pp::adp::IpEndpoint& observed) {
  if (target.family != observed.family) {
    return IsPublicEndpoint(target);
  }
  const size_t n = target.family == pp::adp::IpEndpoint::Family::V4 ? 4 : 8;
  return std::memcmp(target.addr.data(), observed.addr.data(), n) == 0;
}

} // namespace pbr
