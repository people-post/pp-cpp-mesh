#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Who may learn this device's network address (projects/privacy T1, P001): peers we dial or punch
 * toward, answer a punch from, or send our listen addresses to. Everyone else reaches us through a
 * relay. Blocked peers are never allowed.
 */
enum class DirectAudience { Everyone, Contacts, Friendly, Nobody };

const char* DirectAudienceName(DirectAudience audience);
std::optional<DirectAudience> DirectAudienceFromName(std::string_view name);

/**
 * One snapshot of the audience and the contact sets it is judged against. Keys are the identities a
 * caller holds for a peer: device PeerIds and contact identity values (account ids) alike.
 */
struct AddressDisclosurePolicy {
  DirectAudience audience = DirectAudience::Contacts;
  /** Saved contacts (not Blocked). */
  std::unordered_set<std::string> contacts;
  /** Contacts marked Friendly (a subset of `contacts`). */
  std::unordered_set<std::string> friendly;
  std::unordered_set<std::string> blocked;

  bool AllowsDirect(const std::string& peer_key) const;
};

/**
 * Any-thread holder: the product hub publishes a policy whenever the setting or contacts change; mesh
 * and call paths read it where they would disclose our address. Until the first publish nothing is
 * allowed (the default audience with no contacts known).
 */
class AddressDisclosureGate {
public:
  void Publish(AddressDisclosurePolicy policy);
  bool AllowsDirect(const std::string& peer_key) const;
  std::shared_ptr<const AddressDisclosurePolicy> Snapshot() const;

private:
  mutable std::mutex mu_;
  std::shared_ptr<const AddressDisclosurePolicy> policy_ = std::make_shared<const AddressDisclosurePolicy>();
};

/**
 * `gate` may be null: nothing wired (pp-node's own mesh, tests without a hub) discloses as before.
 * Callers holding an optional gate use this rather than testing for null themselves.
 */
inline bool AllowsDirect(const AddressDisclosureGate* gate, const std::string& peer_key) {
  return !gate || gate->AllowsDirect(peer_key);
}

} // namespace pbr
