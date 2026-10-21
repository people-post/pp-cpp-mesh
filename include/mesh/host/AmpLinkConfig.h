#pragma once

#include "amp/link/Types.h"

#include <chrono>

namespace pp::mesh {

/**
 * Keepalive cadences (call-path-resilience K008). Amp ≥ keepalive v2 announces the cadence to the
 * peer, which widens its liveness window to 5/2 × cadence and echoes each keepalive
 * (pp-cpp-amp docs/KEEPALIVE.md), so these only need to beat NAT mapping timeouts:
 * - hot: relay reservations / standby paths — cellular CGNAT can drop idle UDP in 20–30 s;
 * - warm: chat peers — also bounds dead-peer detection to ~1 min.
 */
inline constexpr std::chrono::milliseconds kProductHotKeepaliveInterval{10'000};
inline constexpr std::chrono::milliseconds kProductWarmKeepaliveInterval{25'000};

/**
 * Link-table size by what the host is for. Amp's defaults (48 links, 16 handshakes pending) fit a
 * device. A node that serves others — seeds, relays, DHT, directory — gets one link per client:
 * with the device size a seed's table filled from a handful of devices and labs (dead links stay
 * until their liveness window ends) and it refused everyone ("its link table is full"), so no
 * phone could park on it and every answer waited out the seed park.
 */
enum class AmpLinkTable { Device, Server };
inline constexpr size_t kServerMaxLinks = 2048;
inline constexpr size_t kServerMaxPendingInbound = 128;

/** PeerLinkConfig for the product mesh (MeshHost) — tests use the same config. */
pp::amp::PeerLinkConfig MakeProductAmpLinkConfig(AmpLinkTable table = AmpLinkTable::Device);

} // namespace pp::mesh
