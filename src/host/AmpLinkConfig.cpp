#include "mesh/host/AmpLinkConfig.h"

#include "amp/link/PeerId.h"

#include <algorithm>

namespace pp::mesh {

pp::amp::PeerLinkConfig MakeProductAmpLinkConfig(const AmpLinkTable table, const size_t server_links) {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = pp::amp::PeerIdFromIdentityKey;
  config.keepalive_hot_interval = kProductHotKeepaliveInterval;
  config.keepalive_warm_interval = kProductWarmKeepaliveInterval;
  if (table == AmpLinkTable::Server) {
    config.max_links = server_links > 0 ? server_links : kServerMaxLinks;
    config.max_pending_inbound = std::min(kServerMaxPendingInbound, std::max<size_t>(1, config.max_links / 2));
  }
  return config;
}

size_t AcceptedConnectionsFor(const pp::amp::PeerLinkConfig& config) {
  return std::max(kAmpDefaultAcceptedConnections, config.max_links + config.max_pending_inbound);
}

} // namespace pp::mesh
