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
/**
 * A server's table by default (the operator's mesh.capacity.links overrides it). Measured on a two-core node,
 * Release, clients parked and renewing every 10 s (pp-browser docs/ops/STRESS_LAB.md, 2026-11-07): 2k links 4 % of
 * a core, 4k 13 %, 8k 56 %, ~20 KB each; past ~11k the one pump thread saturates and the node sheds to ~7-8k. The
 * cost grows faster than the count (Amp walks the table each tick), so 8192 keeps an idle table near half a core,
 * with busy / full from ~5.7k / ~7.8k.
 */
inline constexpr size_t kServerMaxLinks = 8192;
inline constexpr size_t kServerMaxPendingInbound = 128;
/** pp-cpp-amp's Endpoint accepts this many connections unless told otherwise (Endpoint::max_accepted_conns_). */
inline constexpr size_t kAmpDefaultAcceptedConnections = 4096;
/**
 * The UDP socket buffers a host others connect to asks for (receive and send): bursts wait in the kernel instead of
 * being dropped while the pump works. Linux grants at most net.core.rmem_max / wmem_max (pp-node's packaging raises
 * them; pp-browser docs/ops/BUILD.md); a device keeps the OS default.
 */
inline constexpr size_t kServerUdpBufferBytes = 8 * 1024 * 1024;

/**
 * PeerLinkConfig for the product mesh (MeshHost) — tests use the same config. `server_links` sizes a Server table
 * (0: kServerMaxLinks); pending handshakes stay under half of it, so a flood still cannot fill the table.
 */
pp::amp::PeerLinkConfig MakeProductAmpLinkConfig(AmpLinkTable table = AmpLinkTable::Device, size_t server_links = 0);

/**
 * How many connections the Amp endpoint accepts for a link table: every link a peer opens is one, plus the handshakes
 * waiting for a slot — never fewer than Amp's own default, so a device's endpoint is as before. (Left at Amp's
 * default, 4096, the endpoint capped a node there whatever its table said.)
 */
size_t AcceptedConnectionsFor(const pp::amp::PeerLinkConfig& config);

} // namespace pp::mesh
