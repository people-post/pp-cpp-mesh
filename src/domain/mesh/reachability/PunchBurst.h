#pragma once

#include "amp/link/PeerLinkManager.h"

#include <string>

namespace pbr {

/** Adapter from MeshRuntime::BurstDialResult for ACP result publish. */
struct PunchBurstResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

/** True when Connected ADP exists for peer_id (not carrier). */
bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id);

} // namespace pbr
