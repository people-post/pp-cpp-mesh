#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pp::mesh {

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

/**
 * What a candidate may be used for (docs/PEER_REACH.md § 3.1). The app decides per source and
 * pushes one list (`MeshHopPolicy`); the mesh reads each role's view from it.
 */
enum MeshReachRole : uint32_t {
  /** Park / reserve, circuit R1 (the rendezvous surface). */
  kReachRoleRendezvous = 1u << 0,
  /** A bootstrap seed: the park waits on these and dials them first. */
  kReachRoleSeed = 1u << 1,
  /** Coordinated punch. A contact introducer is asked before the others. */
  kReachRoleIntroducer = 1u << 2,
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
  /** `MeshReachRole` bits (0: none — a hop ranked for media only). */
  uint32_t roles = 0;
};

} // namespace pp::mesh
