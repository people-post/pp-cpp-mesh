#include "mesh/host/AmpLinkConfig.h"

#include "amp/link/PeerId.h"

namespace pp::mesh {

pp::amp::PeerLinkConfig MakeProductAmpLinkConfig(const AmpLinkTable table) {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = pp::amp::PeerIdFromIdentityKey;
  config.keepalive_hot_interval = kProductHotKeepaliveInterval;
  config.keepalive_warm_interval = kProductWarmKeepaliveInterval;
  if (table == AmpLinkTable::Server) {
    config.max_links = kServerMaxLinks;
    config.max_pending_inbound = kServerMaxPendingInbound;
  }
  return config;
}

} // namespace pp::mesh
