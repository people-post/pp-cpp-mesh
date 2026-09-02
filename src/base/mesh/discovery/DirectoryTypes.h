#pragma once

#include "common/DirectoryTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kDirectoryProtocolId = "/pp-mesh/directory/1.0.0";
inline constexpr int kDirectoryWireVersion = 1;

struct AmpDirectoryServiceConfig {
  std::string local_peer_id;
  /** PeerLinkManager endpoint keys to query for list_mesh_nodes. */
  std::vector<std::string> query_peer_keys;
  /** Control-channel timeout (ms); default 5000. */
  int rpc_timeout_ms = 5000;
  int inbound_ops_per_peer_per_window = 30;
  int inbound_rate_window_seconds = 10;
};

/** Snapshot source for inbound list_mesh_nodes (pp-node / Node host). */
using AmpDirectoryNodesProvider = std::function<std::vector<MeshNodeHit>()>;

} // namespace pbr
