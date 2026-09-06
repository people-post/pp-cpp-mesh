#pragma once

#include "foundation/data/Config.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kDhtProtocolId = "/pp-mesh/dht/1.0.0";
inline constexpr int kDhtWireVersion = 1;

/** Relay caps in DHT records (n2-caps); same vocabulary as directory mesh_node. */
struct PeerRoutingCapabilities {
  bool circuit_relay = false;
  bool media_relay = false;
};

struct PeerRoutingRecord {
  std::string peer_id;
  int64_t seq = 0;
  int64_t ttl_seconds = 3600;
  int64_t issued_at = 0;
  std::vector<std::string> multiaddrs;
  std::optional<PeerRoutingCapabilities> capabilities;
  std::string signature_b64;
  std::string signature_alg = "ml-dsa-65";
};

struct DhtFindPeerResult {
  std::string peer_id;
  PeerRoutingRecord record;
  bool from_cache = false;
};

/** Ops snapshot for pp-node --status / Me→Network debug (n2-hard). */
struct DhtOpsStats {
  bool started = false;
  bool participate = false;
  size_t cached_records = 0;
  uint64_t inbound_find_peer = 0;
  uint64_t inbound_store = 0;
  uint64_t inbound_rate_limited = 0;
  uint64_t store_rejected = 0;
  uint64_t find_peer_issued = 0;
  uint64_t soft_reputation_skips = 0;
  size_t soft_reputation_penalized_peers = 0;
};

struct AmpDhtProtocolConfig {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  std::vector<uint8_t> device_signing_secret;
  std::vector<uint8_t> device_signing_public;
  MeshDhtConfig tunables{};
  /** Registered PeerLinkManager endpoint keys for bootstrap/query targets. */
  std::vector<std::string> query_peer_keys;
  bool participate = false;
  /** Published in signed record when participate (from Node mesh capabilities). */
  bool publish_circuit_relay = false;
  bool publish_media_relay = false;
};

} // namespace pbr
