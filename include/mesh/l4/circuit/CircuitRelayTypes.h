#pragma once

#include "mesh/types/RelayScope.h"
#include "mesh/l4/shared/L4ProtocolIds.h"
#include <string>
#include <unordered_set>

namespace pp::mesh {

/** Provider admission (nf / N023): scope mask + contact PeerIds. */
struct CircuitRelayAdmissionPolicy {
  /** When true and contact_peer_ids non-empty, refuse non-contact dialers (legacy; see serve_scope_mask). */
  bool prefer_contacts_only = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

/** Circuit bridge outcome. */
struct CircuitRelayBridgeResult {
  bool ok = false;
  std::string error;
  std::string resolved_multiaddr;
};

} // namespace pp::mesh
