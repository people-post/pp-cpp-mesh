#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Identity handle kinds on a Contact (D079 / D096).
 *  PeerId = network who; Blockchain (CAIP-10) = find/lookup; RelayUser = v1 route. */
enum class ContactIdKind { RelayUser, PeerId, Blockchain, Custom };

enum class TrustLevel { Unknown, Friendly, Blocked };

struct ContactId {
  ContactIdKind kind = ContactIdKind::RelayUser;
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

/** Copy local/remote into flat mirror fields used by messaging/UI helpers. */
inline void SyncContactMirrors(Contact& contact) {
  contact.display_name = contact.local.display_name;
  contact.server_nickname = contact.remote.nickname;
  contact.ids = contact.remote.ids;
  contact.multiaddrs = contact.remote.multiaddrs;
  contact.trust = contact.local.trust;
}

/** When callers still set flat fields only, lift them into nested before mirroring. */
inline void PromoteFlatFieldsToNested(Contact& contact) {
  const bool nested_empty = contact.local.display_name.empty() && contact.remote.nickname.empty() &&
                            contact.remote.ids.empty() && contact.remote.multiaddrs.empty() &&
                            contact.remote.fetched_at == 0;
  if (!nested_empty) {
    return;
  }
  contact.local.display_name = contact.display_name;
  contact.local.trust = contact.trust;
  contact.remote.nickname = contact.server_nickname;
  contact.remote.ids = contact.ids;
  contact.remote.multiaddrs = contact.multiaddrs;
}

/** Title: local display_name, else remote nickname, else primary relay id. */
inline std::string ContactEffectiveTitle(const Contact& contact) {
  if (!contact.local.display_name.empty()) {
    return contact.local.display_name;
  }
  if (!contact.remote.nickname.empty()) {
    return contact.remote.nickname;
  }
  for (const ContactId& id : contact.remote.ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary && !id.value.empty()) {
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
  std::optional<std::string> signing_public_key_b64;
  std::optional<std::string> kem_public_key_b64;
  std::vector<std::string> multiaddrs;
  /** Initiation floor in pp_credit minor units; missing on wire → 0 (P001). */
  int64_t initiation_floor = 0;
};

} // namespace pbr
