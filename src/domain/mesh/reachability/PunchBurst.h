#pragma once

#include "amp/link/PeerLinkManager.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** Result of a punch sync-window burst dial (L3.25). */
struct PunchBurstResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

/**
 * Dial sanitized peer ADP multiaddrs within a wall-clock window.
 * Uses ephemeral DialKeys (`punch:burst:N:…`) so inbound adopt can own PeerId (A026).
 * Safe to call outside ChannelMux data callbacks (Windows SEH / nested Pump).
 */
PunchBurstResult BurstDialCandidates(pp::amp::PeerLinkManager& links, std::function<void()> io_pump,
                                     const std::vector<std::string>& targets, int window_ms);

/** True when a non-carrier Connected PeerLink exists for peer_id. */
bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id);

} // namespace pbr
