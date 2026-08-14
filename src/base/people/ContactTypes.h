#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Identity handle kinds on a Contact (D079 / D096 / M009).
 *  Account = person (communicating identity); PeerId = device endpoint;
 *  Blockchain (CAIP-10) = find/lookup; RelayUser = Brief route / inbox. */
enum class ContactIdKind { Account, RelayUser, PeerId, Blockchain, Custom };

enum class TrustLevel { Unknown, Friendly, Blocked };

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

/** User annotations — Sync must not overwrite these. */
struct ContactLocal {
  std::string display_name;
  TrustLevel trust = TrustLevel::Unknown;
};

/** Last known directory/lookup snapshot. */
struct ContactRemote {
  std::string nickname;
  std::vector<ContactId> ids;
  std::vector<DirectoryEndpoint> endpoints;
  std::vector<std::string> multiaddrs;
  /** Unix ms when remote was last refreshed; 0 = never synced. */
  int64_t fetched_at = 0;
};

/**
 * Address-book contact: local annotations + remote directory snapshot.
 * Flat `display_name` / `server_nickname` / `ids` / `multiaddrs` / `trust` are mirrors of
 * local/remote for existing call sites; call SyncContactMirrors after mutating nested fields.
 * `overrides` is reserved (always empty this pass) for future peer_id/multiaddr pins.
 */
struct Contact {
  std::string id;
  ContactLocal local;
  ContactRemote remote;
  /** Reserved JSON object for future field pins; unused this pass. */
  bool has_overrides_placeholder = true;

  // Mirrors (kept in sync via SyncContactMirrors):
  std::string display_name;
  std::string server_nickname;
  std::vector<ContactId> ids;
  TrustLevel trust = TrustLevel::Unknown;
  /** Dialable libp2p multiaddrs (must include /p2p/<PeerId>). */
  std::vector<std::string> multiaddrs;
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

/** Copy local/remote into flat mirror fields used by messaging/UI helpers. */
inline void SyncContactMirrors(Contact& contact) {
  FlattenDirectoryEndpoints(contact.remote.ids, contact.remote.multiaddrs, contact.remote.endpoints);
  contact.display_name = contact.local.display_name;
  contact.server_nickname = contact.remote.nickname;
  contact.ids = contact.remote.ids;
  contact.multiaddrs = contact.remote.multiaddrs;
  contact.trust = contact.local.trust;
}

/** When callers still set flat fields only, lift them into nested before mirroring. */
inline void PromoteFlatFieldsToNested(Contact& contact) {
  const bool nested_empty = contact.local.display_name.empty() && contact.remote.nickname.empty() &&
                            contact.remote.ids.empty() && contact.remote.endpoints.empty() &&
                            contact.remote.multiaddrs.empty() && contact.remote.fetched_at == 0;
  if (!nested_empty) {
    return;
  }
  contact.local.display_name = contact.display_name;
  contact.local.trust = contact.trust;
  contact.remote.nickname = contact.server_nickname;
  contact.remote.ids = contact.ids;
  contact.remote.multiaddrs = contact.multiaddrs;
}

/** Title: local display_name, else remote nickname, else Account ID, else relay id. */
inline std::string ContactEffectiveTitle(const Contact& contact) {
  if (!contact.local.display_name.empty()) {
    return contact.local.display_name;
  }
  if (!contact.remote.nickname.empty()) {
    return contact.remote.nickname;
  }
  for (const ContactId& id : contact.remote.ids) {
    if (id.kind == ContactIdKind::Account && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.remote.ids) {
    if (id.kind == ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.remote.ids) {
    if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

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
};

} // namespace pbr
