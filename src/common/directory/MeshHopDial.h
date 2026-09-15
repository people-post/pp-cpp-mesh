#pragma once

#include "common/directory/MeshHopTypes.h"

#include <string>
#include <vector>

namespace pbr {

/** True when both multiaddrs share the same IPv4 /24 (v1 link scope inference). */
bool IsSameIpv4Subnet24(const std::string& multiaddr_a, const std::string& multiaddr_b);

/** True when multiaddr embeds a private IPv4 host (10/8, 172.16/12, 192.168/16). */
bool MultiaddrHasPrivateIpv4Host(const std::string& multiaddr);

/**
 * True when multiaddr embeds a publicly dialable host: public IPv4 or global IPv6.
 * False for private/link-local/ULA/loopback/wildcard and non-IP multiaddrs.
 */
bool MultiaddrHasPublicDialHost(const std::string& multiaddr);

/**
 * Prefer dialable advertise MAs (H002/N013): global `/ip6` > public `/ip4` > other ADP >
 * first non-empty. Used for directory/DHT/contact hop PreferredMultiaddr selection.
 */
std::string PreferredDialMultiaddr(const std::vector<std::string>& multiaddrs);

/**
 * Stable order for PeerLink RegisterEndpoint last-write-wins: worst → best so Preferred
 * lands on global `/ip6` / public `/ip4`.
 */
std::vector<std::string> OrderDialMultiaddrsWorstToBest(std::vector<std::string> multiaddrs);

/**
 * Soft-migrate: prepend local PeerId as hop (AttachAsLocalHop locally).
 * `local_multiaddr` is advertised in CallSfuAttach so remotes can dial this Node — leave empty
 * only in unit tests; production SoftMigrate must pass a LAN/advertise multiaddr.
 * No-op when local_peer_id empty. Dedupes if already present.
 */
std::vector<MeshHopCandidate> PreferLocalMediaHop(std::vector<MeshHopCandidate> ranked,
                                                  const std::string& local_peer_id,
                                                  const std::string& local_multiaddr = {});

} // namespace pbr
