#pragma once

#include <string>

namespace pbr {

/** Client/relay bridge destination (media-hop L3 / Amp circuit). */
struct CircuitBridgeTarget {
  /** Target PeerId base58; required when multiaddr empty. */
  std::string target_peer_id;
  /** Optional explicit multiaddr; relay may still resolve peer_id when empty. */
  std::string target_multiaddr;
  /** Stream protocol on target after bridge (default circuit-relay). */
  std::string target_protocol;
};

} // namespace pbr
