#pragma once

#include "common/Error.h"
#include "lib/amp/link/Types.h"

namespace pbr::test {

/** Derives libp2p-style base58 PeerIds for AMP mesh tests (implemented in .cpp; links pp_base_peer_id). */
pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key);

/** Wires `peer_id_from_identity` via `DeriveTestPeerId` for MemoryDatagramIo harnesses. */
pp::amp::PeerLinkConfig AmpMeshTestLinkConfig();

} // namespace pbr::test
