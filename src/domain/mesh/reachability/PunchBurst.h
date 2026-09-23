#pragma once

#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** Result of a punch sync-window burst dial (L3.25). Prefer MeshRuntime::BurstDial. */
struct PunchBurstResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

/**
 * Sync test-only burst (steady_clock poll). Product uses MeshRuntime::BurstDial.
 * Nesting rule: must not run under ChannelMux or MeshRuntime::DrainPostedIo (Windows SEH).
 */
PunchBurstResult BurstDialCandidates(pp::amp::PeerLinkManager& links, std::function<void()> io_pump,
                                     const std::vector<std::string>& targets, int window_ms);

/** True when a non-carrier Connected PeerLink exists for peer_id (ADP IsConnectedToPeerId). */
bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id);

} // namespace pbr
