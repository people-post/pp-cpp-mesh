#include "domain/mesh/reachability/PunchBurst.h"

namespace pbr {

bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id) {
  if (peer_id.empty()) {
    return false;
  }
  // Nested/circuit carrier links must not short-circuit punch (L3.25c upgrade-from-circuit).
  return links.IsConnectedToPeerId(peer_id);
}

} // namespace pbr
