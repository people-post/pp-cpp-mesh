#pragma once

#include "common/directory/RelayScope.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>
#include <unordered_set>

namespace libp2p::connection {
class Stream;
}

namespace pbr {

inline constexpr const char* kCircuitProtocolId = "/pp-browser/circuit/1.0.0";
inline constexpr const char* kCircuitRelayProtocolId = kCircuitProtocolId;

/** Product wire id for nested Session over circuit (Amp plumbing; configured into PeerLinkManager). */
inline constexpr const char* kCircuitCarrierProtocolId = "/pp-browser/circuit-carrier/1.0.0";

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
