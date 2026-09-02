#pragma once

/**
 * Shared directory / phone-book vocabulary (N027 / N029 / M009).
 * Domain peers (net, mesh, people, messaging) include this instead of each other.
 */

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Identity handle kinds on a Contact / directory hit (D079 / D096 / M009).
 *  Account = person (communicating identity); PeerId = device endpoint;
 *  Blockchain (CAIP-10) = find/lookup; RelayUser = Brief route / inbox. */
enum class ContactIdKind { Account, RelayUser, PeerId, Blockchain, Custom };

/** One device under an Account ID (M017). `updated_at` is Unix ms. */
struct DirectoryEndpoint {
  std::string peer_id;
  std::vector<std::string> multiaddrs;
  int64_t updated_at = 0;
};

struct ContactId {
  ContactIdKind kind = ContactIdKind::Account;
  std::string value;
  bool primary = false;
};

/** Public directory icon reference (plaintext CDN). */
struct ProfileIconRef {
  std::string url;
  std::string blob_id;
  std::string kind;

  bool empty() const { return url.empty() && blob_id.empty(); }
};

struct DirectoryHit {
  std::string hit_id;
  std::string display_name;
  std::string nickname;
  std::vector<ContactId> ids;
  std::optional<std::string> account_id;
  std::optional<std::string> signing_public_key_b64;
  std::optional<std::string> kem_public_key_b64;
  std::vector<DirectoryEndpoint> endpoints;
  std::vector<std::string> multiaddrs;
  /** Initiation floor in pp_credit minor units; missing on wire → 0 (P001). */
  int64_t initiation_floor = 0;
  std::optional<ProfileIconRef> icon;
  /** person | mesh_node; empty → treat as person for people search (N029). */
  std::string entity_kind;
  int64_t seq = 0;
  std::string expires_at;
  /** Optional capability ads when provider includes them (N029). */
  bool has_capabilities = false;
  bool circuit_relay = false;
  bool media_relay = false;
  bool dht = false;
  bool ledger_gateway = false;
};

struct MeshCapabilitiesAd {
  bool circuit_relay = false;
  bool media_relay = false;
  bool dht = false;
  /** N029 Phase C prep — directory/config only until ledger transport ships. */
  bool ledger_gateway = false;
};

struct MeshNodeHit {
  std::string relay_user_id;
  std::optional<std::string> account_id;
  std::optional<std::string> nickname;
  std::vector<DirectoryEndpoint> endpoints;
  MeshCapabilitiesAd capabilities;
  std::string expires_at;
  /** person | mesh_node; default mesh_node for ListMeshNodes rows. */
  std::string entity_kind;
  /** Monotonic directory/chain seq; 0 when provider omits (N029). */
  int64_t seq = 0;
  std::optional<std::string> signing_public_key_b64;
  std::optional<std::string> kem_public_key_b64;
};

inline const DirectoryEndpoint* PreferredDirectoryEndpoint(const std::vector<DirectoryEndpoint>& endpoints) {
  if (endpoints.empty()) {
    return nullptr;
  }
  const DirectoryEndpoint* best = &endpoints.front();
  for (const DirectoryEndpoint& endpoint : endpoints) {
    if (endpoint.updated_at > best->updated_at) {
      best = &endpoint;
    }
  }
  return best;
}

inline bool DirectoryEndpointHasPeerId(const std::vector<ContactId>& ids, const std::string& peer_id) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::PeerId && id.value == peer_id) {
      return true;
    }
  }
  return false;
}

inline bool DirectoryHasMultiaddr(const std::vector<std::string>& multiaddrs, const std::string& multiaddr) {
  for (const std::string& existing : multiaddrs) {
    if (existing == multiaddr) {
      return true;
    }
  }
  return false;
}

/** Merge endpoint Peer IDs into `ids` and multiaddrs (newest first). Does not drop other id kinds. */
inline void FlattenDirectoryEndpoints(std::vector<ContactId>& ids, std::vector<std::string>& multiaddrs,
                                      const std::vector<DirectoryEndpoint>& endpoints) {
  std::vector<const DirectoryEndpoint*> ordered;
  ordered.reserve(endpoints.size());
  for (const DirectoryEndpoint& endpoint : endpoints) {
    ordered.push_back(&endpoint);
  }
  std::sort(ordered.begin(), ordered.end(), [](const DirectoryEndpoint* a, const DirectoryEndpoint* b) {
    return a->updated_at > b->updated_at;
  });
  std::vector<std::string> merged_multiaddrs;
  for (const DirectoryEndpoint* endpoint : ordered) {
    if (!endpoint->peer_id.empty() && !DirectoryEndpointHasPeerId(ids, endpoint->peer_id)) {
      ids.push_back({ContactIdKind::PeerId, endpoint->peer_id, false});
    }
    for (const std::string& multiaddr : endpoint->multiaddrs) {
      if (!multiaddr.empty() && !DirectoryHasMultiaddr(merged_multiaddrs, multiaddr)) {
        merged_multiaddrs.push_back(multiaddr);
      }
    }
  }
  if (!endpoints.empty()) {
    multiaddrs = std::move(merged_multiaddrs);
  }
}

}  // namespace pbr
