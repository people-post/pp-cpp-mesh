#pragma once

#include "mesh/types/MeshConfigTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace pp::mesh {

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
  /**
   * The signer's ML-DSA-65 public key (base64): with it any node can check the record — the key
   * derives `peer_id`, and the signature holds. Not part of the signed bytes (the PeerId binds it).
   */
  std::string public_key_b64;
};

/**
 * A `find_peer_result.closer_peers` entry (docs/DHT.md § 4.3): a DHT participant near the target
 * key, from the responder's routing table — a hint to ask, never a table entry by itself.
 */
struct DhtCloserPeer {
  std::string peer_id;
  /** Ends in `/p2p/<peer_id>`, so dialling it authenticates the peer. */
  std::string multiaddr;
};

/**
 * How many `closer_peers` a responder offers (the contract's `k_closer`): k, as in Kademlia — a
 * walk to the k closest (publishing) only meets them all if each answer can name that many.
 */
inline constexpr size_t kDhtCloserPeers = 20;

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
  /** Routing-table contacts (docs/DHT.md § 4.2). */
  size_t routing_contacts = 0;
  uint64_t inbound_find_peer = 0;
  uint64_t inbound_store = 0;
  uint64_t inbound_rate_limited = 0;
  uint64_t store_rejected = 0;
  uint64_t find_peer_issued = 0;
  uint64_t self_publishes = 0;
  size_t last_publish_targets = 0;
  uint64_t soft_reputation_skips = 0;
  size_t soft_reputation_penalized_peers = 0;
};

struct AmpDhtProtocolConfig {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  /** Live addresses to publish (they follow network changes); unset: `listen_multiaddrs`. */
  std::function<std::vector<std::string>()> current_listen_multiaddrs;
  /** The publish gate's reachability half: false while reachability is Blocked. Unset: open. */
  std::function<bool()> may_publish;
  /**
   * Liveness (docs/DHT.md D5): the reach record holds this peer down — not dialled for a lookup,
   * and out of the routing table. A peer whose link is up counts as alive whatever it says.
   */
  std::function<bool(const std::string& peer_id)> reach_held_down;
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

} // namespace pp::mesh
