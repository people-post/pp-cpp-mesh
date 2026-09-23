#pragma once

#include "amp/link/PeerLinkManager.h"

#include <chrono>
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
 *
 * Nesting rule: must not run under ChannelMux or MeshRuntime::DrainPostedIo (Windows SEH).
 * Prefer BurstDialCandidatesAsync + settle_on(PostDeferred). Sync path is test-only without IoPost.
 */
PunchBurstResult BurstDialCandidates(pp::amp::PeerLinkManager& links, std::function<void()> io_pump,
                                     const std::vector<std::string>& targets, int window_ms);

/** Amp-clock delayed task — typically MeshRuntime::PostAfter. */
using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

/**
 * Async burst: PostToIo polls for wins; optional PostAfter arms the sync window on Amp clock.
 * Settles only on PeerId-visible non-carrier Connected (FindLinkByPeerId).
 *
 * AbortInflightDial + on_done run via settle_on when provided (MeshRuntime::PostDeferred).
 */
void BurstDialCandidatesAsync(pp::amp::PeerLinkManager& links,
                              std::function<void(std::function<void()>)> post_io,
                              const std::vector<std::string>& targets, int window_ms,
                              std::function<void(PunchBurstResult)> on_done,
                              std::function<void(std::function<void()>)> settle_on = {},
                              IoAfter post_after = {});

/** True when a non-carrier Connected PeerLink exists for peer_id. */
bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id);

} // namespace pbr
