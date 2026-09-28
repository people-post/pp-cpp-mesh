#pragma once

#include <chrono>

namespace pbr {

/**
 * The device's network attachment changed (k5). Mesh vocabulary only: the platform's monitor
 * (foundation/platform/NetworkMonitor) is translated by the product assembler.
 */
struct LocalNetworkChange {
  bool was_online = false;
  bool online = false;
  /** Default-route interfaces / addresses differ (not just cost or transport labels). */
  bool attachment_changed = false;
};

/** What the mesh does about it. */
struct LocalNetworkReaction {
  /** Probe every direct link now; evict those that do not answer within Amp's grace. */
  bool probe_links = false;
  /** Learn our addresses again (observed endpoint, UPnP) and refresh advertised / punch addrs. */
  bool reprobe_reachability = false;
};

/**
 * Offline: nothing — probes into no route would drop every link at once, and they may survive a
 * short outage (the next online change probes them). Online on a new attachment, or back online:
 * probe links and re-probe reachability. A cost / label-only change needs nothing from the mesh.
 */
LocalNetworkReaction DecideLocalNetworkReaction(const LocalNetworkChange& change);

/** Re-probe reachability once link probing settled (past Amp's 2 s network-change grace). */
inline constexpr std::chrono::milliseconds kReachabilityReprobeAfterNetworkChange{2500};

} // namespace pbr
