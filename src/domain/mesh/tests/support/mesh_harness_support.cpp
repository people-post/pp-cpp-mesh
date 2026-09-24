#include "domain/mesh/tests/support/mesh_harness_support.h"

#include "domain/mesh/host/AmpLinkConfig.h"
#include "foundation/identity/PeerIdUtil.h"

namespace pbr::test {

pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key) {
  return PeerIdFromMlDsaPublicKey(identity_public_key);
}

pp::amp::PeerLinkConfig AmpMeshTestLinkConfig() {
  // Same tuning as product (keepalive cadence vs liveness matters for link survival tests).
  return MakeProductAmpLinkConfig();
}

} // namespace pbr::test
