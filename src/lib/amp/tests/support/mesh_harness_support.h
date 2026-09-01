#pragma once

#include "common/Error.h"
#include "lib/amp/link/Types.h"

namespace pbr::test {

/** Deterministic test PeerIds for AMP harnesses (stub in lib/amp; L4 tests override in p2p/tests/support). */
pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key);

/** Wires `peer_id_from_identity` via `DeriveTestPeerId` for MemoryDatagramIo harnesses. */
pp::amp::PeerLinkConfig AmpMeshTestLinkConfig();

} // namespace pbr::test
