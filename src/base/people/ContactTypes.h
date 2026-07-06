#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pbr {

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
};

struct DirectoryHit {
  std::string hit_id;
  std::string display_name;
  std::string nickname;
  std::vector<ContactId> ids;
  std::optional<std::string> signing_public_key_b64;
};

} // namespace pbr
