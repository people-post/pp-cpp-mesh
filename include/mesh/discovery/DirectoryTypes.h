#pragma once

#include "mesh/types/DirectoryTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pp::mesh {

inline constexpr const char* kDirectoryProtocolId = "/pp-mesh/directory/1.0.0";
inline constexpr int kDirectoryWireVersion = 1;

struct AmpDirectoryProtocolConfig {
  std::string local_peer_id;
  /** PeerLinkManager endpoint keys to query for list_mesh_nodes. */
  std::vector<std::string> query_peer_keys;
  /**
   * Which of `query_peer_keys` a list asks (in parallel; first answer wins). The owner's choice
   * from what reaching them has gone like (`MeshConnectivity::ChoosePeers`: up first, none known
   * gone). None: every key.
   */
  std::function<std::vector<std::string>(const std::vector<std::string>& keys)> choose_query_peers;
  /** Control-channel timeout (ms); default 5000. */
  int rpc_timeout_ms = 5000;
  int inbound_ops_per_peer_per_window = 30;
  int inbound_rate_window_seconds = 10;
};

/** Snapshot source for inbound list_mesh_nodes (pp-node / Node host). */
using AmpDirectoryNodesProvider = std::function<std::vector<MeshNodeHit>()>;

} // namespace pp::mesh
