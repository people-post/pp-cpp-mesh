#pragma once

#include <cstddef>

namespace pbr {

/** Active helper load aggregates for ambient chrome (network-status-chrome s3 / S008–S009). */
struct CircuitRelayRuntimeStats {
  /** Live inbound bridges this Node is hosting (≈ helped dialer clients). */
  size_t active_bridges = 0;
};

struct MediaRelayRuntimeStats {
  /** Non-empty HostSessions being served. */
  size_t active_sessions = 0;
  /** Sum of participants across those sessions (aggregates only — no PeerIds). */
  size_t active_participants = 0;
};

struct RelayRuntimeStats {
  CircuitRelayRuntimeStats circuit;
  MediaRelayRuntimeStats media;
  bool circuit_serving = false;
  bool media_serving = false;
};

} // namespace pbr
