#include "domain/mesh/host/AmpLinkConfig.h"

#include "amp/L1/Types.h"
#include "foundation/identity/PeerIdUtil.h"

namespace pbr {

static_assert(kProductHotKeepaliveInterval.count() < pp::adp::kAliveTimeoutMs,
              "hot keepalive must beat the peer's ADP liveness window");

pp::amp::PeerLinkConfig MakeProductAmpLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pp::amp::ByteVector& identity_public_key) -> std::string {
    auto peer_id = PeerIdFromMlDsaPublicKey(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  config.keepalive_hot_interval = kProductHotKeepaliveInterval;
  return config;
}

} // namespace pbr
