#pragma once

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

struct Contact {
  std::string id;
  std::string display_name;
  std::string server_nickname;
  std::vector<ContactId> ids;
  TrustLevel trust = TrustLevel::Unknown;
  /** Dialable libp2p multiaddrs (must include /p2p/<PeerId>). */
  std::vector<std::string> multiaddrs;
};

struct DirectoryHit {
  std::string hit_id;
  std::string display_name;
  std::string nickname;
  std::vector<ContactId> ids;
  std::optional<std::string> signing_public_key_b64;
  std::optional<std::string> kem_public_key_b64;
  std::vector<std::string> multiaddrs;
};

} // namespace pbr
