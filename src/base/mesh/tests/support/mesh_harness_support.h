#pragma once

#include "lib/amp/link/Types.h"

#include "lib/amp/AmpRoe.h"

namespace pbr::test {

/** Derives libp2p-style base58 PeerIds for AMP mesh tests (implemented in .cpp; links pp_base_peer_id). */
Roe<std::string> DeriveTestPeerId(const pbr::amp::ByteVector& identity_public_key);

/** Wires `peer_id_from_identity` via `DeriveTestPeerId` for MemoryDatagramIo harnesses. */
amp::PeerLinkConfig AmpMeshTestLinkConfig();

} // namespace pbr::test
