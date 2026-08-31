#include "base/mesh/link/tests/mesh_harness_support.h"

#include "base/p2p/PeerIdUtil.h"

namespace pbr::test {

amp::PeerLinkConfig AmpMeshTestLinkConfig() {
  amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const ByteVector& identity_public_key) -> std::string {
    auto peer_id = PeerIdFromMlDsaPublicKey(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  return config;
}

} // namespace pbr::test
