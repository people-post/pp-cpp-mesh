#pragma once

#include "base/people/RelayScope.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>
#include <unordered_set>

namespace libp2p::connection {
class Stream;
}

namespace pbr {

inline constexpr const char* kCircuitRelayProtocolId = "/pp-browser/circuit-relay/1.0.0";

/** Provider admission (nf / N023): scope mask + contact PeerIds. */
struct CircuitRelayAdmissionPolicy {
  /** When true and contact_peer_ids non-empty, refuse non-contact dialers (legacy; see serve_scope_mask). */
  bool prefer_contacts_only = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

/** Circuit bridge outcome (Amp product path ignores `stream`). */
struct CircuitRelayBridgeResult {
  bool ok = false;
  std::string error;
  std::string resolved_multiaddr;
  std::shared_ptr<libp2p::connection::Stream> stream;
};

} // namespace pbr
