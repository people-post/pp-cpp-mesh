#pragma once

#include "common/Error.h"

#include <optional>
#include <string>

namespace pbr {

class Libp2pHost;
class PeerSessionManager;

/** Client/relay bridge destination (media-hop L3). */
struct CircuitBridgeTarget {
  /** Target PeerId base58; required when multiaddr empty. */
  std::string target_peer_id;
  /** Optional explicit multiaddr; relay may still resolve peer_id when empty. */
  std::string target_multiaddr;
  /** Stream protocol on target after bridge (default circuit-relay). */
  std::string target_protocol;
};

/** Resolve a dial multiaddr for circuit-bridge target (book → host repo). */
std::optional<std::string> ResolveCircuitTargetMultiaddr(PeerSessionManager& sessions, Libp2pHost& host,
                                                         const std::string& target_peer_id);

/** Merge target fields into a dialable multiaddr + canonical peer id. */
Roe<std::pair<std::string, std::string>> NormalizeCircuitBridgeTarget(PeerSessionManager& sessions,
                                                                      Libp2pHost& host,
                                                                      const CircuitBridgeTarget& target);

} // namespace pbr
