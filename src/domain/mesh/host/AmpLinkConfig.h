#pragma once

#include "amp/link/Types.h"

#include <chrono>

namespace pbr {

/**
 * Hot keepalive cadence for product links. Must stay below the peer's ADP liveness window
 * (`pp::adp::kAliveTimeoutMs`, 5 s): the far end of our link is usually a cold inbound link that
 * evicts us after 5 s without RX, whatever tier we hold locally (dogfood 2026-09-24: reserved
 * relay links evicted → "Couldn't connect"). Amp's default hot interval is 20 s.
 */
inline constexpr std::chrono::milliseconds kProductHotKeepaliveInterval{2000};

/** PeerLinkConfig for the product mesh (MeshHost) — tests use the same config. */
pp::amp::PeerLinkConfig MakeProductAmpLinkConfig();

} // namespace pbr
