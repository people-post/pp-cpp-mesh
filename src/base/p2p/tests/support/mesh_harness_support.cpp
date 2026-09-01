#include "base/p2p/tests/support/mesh_harness_support.h"

#include "base/p2p/PeerIdUtil.h"

namespace pbr::test {

pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key) {
  return PeerIdFromMlDsaPublicKey(identity_public_key);
}

pp::amp::PeerLinkConfig AmpMeshTestLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pp::amp::ByteVector& identity_public_key) -> std::string {
    auto peer_id = DeriveTestPeerId(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  return config;
}

} // namespace pbr::test
