#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** Affinity class for hop scoring (N014 / N020). */
enum class MeshHopAffinity {
  Contact = 0,
  DirectoryNode = 1,
  DhtDiscovered = 2,
  OrgSeed = 3,
  Other = 4,
};

/** Mirrors ReachabilityStatus for provider caps without requiring mesh headers in callers. */
enum class MeshReachabilityClass {
  Unknown,
  Reachable,
  OutboundOnly,
  Blocked,
};

struct MeshHopCandidate {
  std::string peer_id;
  /** Dial multiaddr including `/p2p/<PeerId>` when known. */
  std::string multiaddr;
  MeshHopAffinity affinity = MeshHopAffinity::Other;
  bool dialable = true;
  bool recently_failed = false;
  /** 0..1 residual capacity estimate; 1 = fully free. */
  double residual_capacity = 1.0;
  /** From mesh directory capabilities (n-dir); used for media/circuit filters. */
  bool advertises_media_relay = false;
  bool advertises_circuit_relay = false;
};

} // namespace pbr
