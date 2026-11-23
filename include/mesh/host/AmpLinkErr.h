#pragma once

#include "amp/link/PeerLinkManager.h"
#include "mesh/host/MeshPorts.h"

namespace pp::mesh {

/**
 * Amp's link-error code as the mesh port's: the code of the same name (docs/ERRORS.md). Exhaustive — an Amp
 * code this does not name fails mesh's build — and the values are asserted equal, so the two cannot drift. A
 * value that is no Amp enumerator reads as `Generic`.
 */
IChatPeerLinks::Err ToMeshLinkErr(pp::amp::PeerLinkManager::Err code);

/** An Amp link failure as the mesh port's: its code by ToMeshLinkErr, its message as it is (an adapter). */
IChatPeerLinks::Failure ToMeshLinkFailure(const pp::amp::PeerLinkManager::Failure& failure);

} // namespace pp::mesh
