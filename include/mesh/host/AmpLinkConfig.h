#pragma once

#include "amp/link/Types.h"

#include <chrono>

namespace pbr {

/**
 * Keepalive cadences (call-path-resilience K008). Amp ≥ keepalive v2 announces the cadence to the
 * peer, which widens its liveness window to 5/2 × cadence and echoes each keepalive
 * (pp-cpp-amp docs/KEEPALIVE.md), so these only need to beat NAT mapping timeouts:
 * - hot: relay reservations / standby paths — cellular CGNAT can drop idle UDP in 20–30 s;
 * - warm: chat peers — also bounds dead-peer detection to ~1 min.
 */
inline constexpr std::chrono::milliseconds kProductHotKeepaliveInterval{10'000};
inline constexpr std::chrono::milliseconds kProductWarmKeepaliveInterval{25'000};

/** PeerLinkConfig for the product mesh (MeshHost) — tests use the same config. */
pp::amp::PeerLinkConfig MakeProductAmpLinkConfig();

} // namespace pbr
