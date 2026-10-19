#include "domain/mesh/reachability/punch/PunchLinkOps.h"

#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/reachability/punch/PunchLogic.h"
#include "common/PbrCompat.h"

namespace pbr {

pp::amp::ChannelPolicy PunchJsonChannelPolicy(std::chrono::milliseconds read_timeout) {
  auto policy = pp::amp::ControlJsonChannelPolicy(read_timeout);
  policy.read_once = false;
  return policy;
}

std::chrono::milliseconds PunchRemainingTimeout(const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

PunchBurstResult ToPunchBurst(pp::amp::BurstDialResult r) {
  PunchBurstResult out;
  out.ok = r.ok;
  out.dialed = std::move(r.dialed);
  out.error = std::move(r.error);
  return out;
}

void PublishIfPunchConnected(pp::amp::PeerLinkManager& links, const std::string& known_peer_id,
                             PunchBurstResult& burst) {
  std::string peer_id = known_peer_id;
  if (peer_id.empty() && !burst.dialed.empty()) {
    if (auto parsed = pp::amp::ParseAdpMultiaddr(burst.dialed)) {
      peer_id = parsed->peer_id;
    }
  }
  if (!burst.ok && PeerAlreadyConnectedDirect(links, peer_id)) {
    burst.ok = true;
  }
  if (!burst.ok) {
    return;
  }
  std::string winner = burst.dialed;
  if (winner.empty() && !peer_id.empty()) {
    if (auto ma = links.PreferredMultiaddr(peer_id)) {
      winner = *ma;
    }
  }
  // Last resort: any endpoint record whose peer_id matches the authenticated PeerId.
  if (winner.empty() && !peer_id.empty()) {
    if (auto* link = links.FindLinkByPeerId(peer_id)) {
      if (auto ma = links.PreferredMultiaddr(link->PeerKey())) {
        winner = *ma;
      }
    }
  }
  if (winner.empty()) {
    return;
  }
  PublishPunchWinnerAddrs(links, peer_id, winner);
  burst.dialed = winner;
}


PunchFailure WrapPunchLinkFailure(
    const pp::amp::PeerLinkManager::Failure& child) {
  using Err = PunchErr;
  using Failure = PunchFailure;
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("punch: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("punch: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("punch: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::PunchFailed, detail::AppendFrom("punch: dual-dial lost", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("punch: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("punch: link error", "link", child.message));
  }
}

} // namespace pbr
