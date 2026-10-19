#pragma once

// Configuration types the mesh owns (capabilities, DHT, media relay, pricing, directory rows). Pure
// data with no app dependencies: the mesh library takes them without the app's Config, which embeds
// them in MeshConfig (pp-cpp-mesh extraction, docs/architecture/MESH.md).

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

struct MeshCapabilities {
  /**
   * Host circuit-relay bridge for NAT'd peers (n3 / N009).
   * Node + this flag → ServeInbound / publish. Clients consume org/contact
   * circuit for dialability without enabling this (CallStack AmpCircuitHopReach).
   */
  bool circuit_relay = false;
  /** Host blind media forwarder (n4-media / N018). Default on for Node hosts. */
  bool media_relay = true;
  /** Participate in AMP Kademlia peer routing (n2). Default off; Node only. */
  bool dht = false;
  /**
   * Advertise as opaque ledger RPC gateway (N029 Phase C prep).
   * Default off; no chain runtime required to store the flag.
   */
  bool ledger_gateway = false;
};

/** Tunables for mesh DHT (n2). Ignored when capabilities.dht is false. */
struct MeshDhtConfig {
  /** TTL for self peer_routing records (seconds). Re-publish at ttl/2. */
  int record_ttl_seconds = 3600;
  /** FIND_PEER RPC timeout (milliseconds). */
  int find_peer_timeout_ms = 5000;
  /** Max in-flight FIND_PEER lookups per process. */
  int max_concurrent_lookups = 4;
  /** Kademlia k (bucket size target). Wire default 20 — override for lab only. */
  int k_bucket_size = 20;
  /** Inbound FIND_PEER/STORE grants per remote peer per window (n2-hard). */
  int inbound_ops_per_peer_per_window = 60;
  /** Sliding window length for inbound rate limit (seconds). */
  int inbound_rate_window_seconds = 60;
  /** Soft-reputation: skip query peers after this many bad FIND_PEER replies. */
  int soft_reputation_penalty_threshold = 3;
  /** Soft-reputation cooldown after penalty (seconds). */
  int soft_reputation_cooldown_seconds = 300;
};

/** ↑/↓ ceilings for media_relay (N019). 0 = unbounded / ops default. */
struct MediaRelayBudgetConfig {
  int64_t node_capacity_up_bps = 0;
  int64_t node_capacity_down_bps = 0;
  int64_t max_session_up_bps = 0;
  int64_t max_session_down_bps = 0;
  int64_t default_per_user_up_bps = 0;
  int64_t default_per_user_down_bps = 0;
};

/**
 * Which video levels this node's media_relay carries per publisher (peer-scoped-broadcast B009).
 * Levels are opaque ordered integers (docs/contracts/MEDIA_CHANNELS.md).
 */
struct MediaRelayVideoConfig {
  /** Levels served; empty = any. */
  std::vector<int> serve_levels;
  /** At most this many levels per publisher; 0 = no limit. */
  int carry_levels = 0;
  /** Refuse a publisher offering none of `serve_levels` (default: carry its closest level). */
  bool strict = false;
};

/**
 * Per-capability relay pricing (N010 / P001).
 * Protocol branches on rate (== 0 free); mode is a UX label only.
 */
struct RelayPricingConfig {
  std::string mode = "volunteer"; // UX label: volunteer | paid
  double rate = 0.0;
};

struct MeshPricingConfig {
  RelayPricingConfig media_relay;
};

/** Cached infra row from GET /v1/mesh/nodes (N027 / n-dir / N029). */
struct MeshDirectoryNode {
  std::string peer_id;
  std::vector<std::string> multiaddrs;
  bool circuit_relay = false;
  bool media_relay = false;
  bool dht = false;
  bool ledger_gateway = false;
  std::string account_id;
  std::string entity_kind;
  int64_t seq = 0;
  std::string expires_at;
  std::string nickname;
};

} // namespace pbr
