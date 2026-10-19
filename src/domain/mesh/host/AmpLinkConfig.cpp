#include "domain/mesh/host/AmpLinkConfig.h"

#include "amp/link/PeerId.h"

namespace pbr {

pp::amp::PeerLinkConfig MakeProductAmpLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = pp::amp::PeerIdFromIdentityKey;
  config.keepalive_hot_interval = kProductHotKeepaliveInterval;
  config.keepalive_warm_interval = kProductWarmKeepaliveInterval;
  return config;
}

} // namespace pbr
