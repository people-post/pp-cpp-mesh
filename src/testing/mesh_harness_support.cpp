#include "mesh/testing/mesh_harness_support.h"

#include "mesh/host/AmpLinkConfig.h"
#include "crypto/PeerId.h"

namespace pp::mesh::test {

pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key) {
  return pp::PeerIdFromMlDsaPublicKey(identity_public_key);
}

pp::amp::PeerLinkConfig AmpMeshTestLinkConfig() {
  // Same tuning as product (keepalive cadence vs liveness matters for link survival tests).
  return MakeProductAmpLinkConfig();
}

} // namespace pp::mesh::test
