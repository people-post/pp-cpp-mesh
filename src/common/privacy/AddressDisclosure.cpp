#include "common/privacy/AddressDisclosure.h"

#include "common/PbrCompat.h"

namespace pbr {

const char* DirectAudienceName(const DirectAudience audience) {
  switch (audience) {
  case DirectAudience::Everyone:
    return "everyone";
  case DirectAudience::Contacts:
    return "contacts";
  case DirectAudience::Friendly:
    return "friendly";
  case DirectAudience::Nobody:
    return "nobody";
  }
  return "contacts";
}

std::optional<DirectAudience> DirectAudienceFromName(const std::string_view name) {
  if (name == "everyone") {
    return DirectAudience::Everyone;
  }
  if (name == "contacts") {
    return DirectAudience::Contacts;
  }
  if (name == "friendly") {
    return DirectAudience::Friendly;
  }
  if (name == "nobody") {
    return DirectAudience::Nobody;
  }
  return std::nullopt;
}

const char* InboundAudienceName(const InboundAudience audience) {
  switch (audience) {
  case InboundAudience::Everyone:
    return "everyone";
  case InboundAudience::ContactsOnly:
    return "contacts_only";
  case InboundAudience::Nobody:
    return "nobody";
  }
  return "everyone";
}

InboundAudience InboundAudienceFromName(const std::string_view name, const InboundAudience fallback) {
  if (name == "everyone") {
    return InboundAudience::Everyone;
  }
  if (name == "contacts_only") {
    return InboundAudience::ContactsOnly;
  }
  if (name == "nobody") {
    return InboundAudience::Nobody;
  }
  return fallback;
}

bool AddressDisclosurePolicy::AllowsDirect(const std::string& peer_key) const {
  if (peer_key.empty() || blocked.contains(peer_key)) {
    return false;
  }
  switch (audience) {
  case DirectAudience::Everyone:
    return true;
  case DirectAudience::Contacts:
    return contacts.contains(peer_key);
  case DirectAudience::Friendly:
    return friendly.contains(peer_key);
  case DirectAudience::Nobody:
    return false;
  }
  return false;
}

void AddressDisclosureGate::Publish(AddressDisclosurePolicy policy) {
  auto next = std::make_shared<const AddressDisclosurePolicy>(std::move(policy));
  std::lock_guard lock(mu_);
  policy_ = std::move(next);
}

bool AddressDisclosureGate::AllowsDirect(const std::string& peer_key) const {
  return Snapshot()->AllowsDirect(peer_key);
}

std::shared_ptr<const AddressDisclosurePolicy> AddressDisclosureGate::Snapshot() const {
  std::lock_guard lock(mu_);
  return policy_;
}

} // namespace pbr
