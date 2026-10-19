#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

/** Active helper load aggregates for ambient chrome (network-status-chrome s3 / S008–S009). */
struct CircuitRelayRuntimeStats {
  /** Live inbound bridges this Node is hosting (≈ helped dialer clients). */
  size_t active_bridges = 0;
  /** Tunnels still being set up (far leg not bridged yet). */
  size_t pending_tunnels = 0;
  /** Answerers parked here (op=reserve). */
  size_t reservations = 0;
  /** Bytes spliced through bridges since start (closed ones and the open ones so far). */
  uint64_t bytes_relayed = 0;
};

struct MediaRelayRuntimeStats {
  /** Non-empty HostSessions being served. */
  size_t active_sessions = 0;
  /** Sum of participants across those sessions (aggregates only — no PeerIds). */
  size_t active_participants = 0;
  /**
   * Frame bytes this server relayed for others since start: from a remote participant to a remote
   * participant (not this node's own media in or out; the `pp_media_relay_*` counters count all).
   */
  uint64_t bytes_relayed = 0;
};

struct RelayRuntimeStats {
  CircuitRelayRuntimeStats circuit;
  MediaRelayRuntimeStats media;
  bool circuit_serving = false;
  bool media_serving = false;
};

} // namespace pbr
