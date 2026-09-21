#pragma once

#include "amp/link/PeerLinkManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace pbr {

/**
 * Convert a steady_clock wall deadline to an absolute Amp-clock deadline for
 * PeerLinkManager::WhenChannelOpen. Never pass steady_clock::time_since_epoch() —
 * Amp uses Endpoint::GetClock().NowMs() (often a virtual clock starting near 1e6).
 */
inline int64_t AmpDeadlineFromSteady(pp::amp::PeerLinkManager& links,
                                     const std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  const int64_t now = links.GetEndpoint().GetClock().NowMs();
  return now + std::max<int64_t>(0, rem.count());
}

/** WhenChannelOpen with a steady_clock wall deadline (harness / product parks). */
inline void AmpWhenChannelOpen(pp::amp::PeerLinkManager& links, const std::string& peer_key,
                               const uint32_t channel_id, const std::chrono::steady_clock::time_point deadline,
                               std::function<void(bool open)> done) {
  links.WhenChannelOpen(peer_key, channel_id, AmpDeadlineFromSteady(links, deadline), std::move(done));
}

} // namespace pbr
