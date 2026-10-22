#pragma once

#include "mesh/types/RelayScope.h"
#include "mesh/l4/shared/L4ProtocolIds.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pp::mesh {

/** Provider admission (nf / N023): scope mask + contact PeerIds. */
struct CircuitRelayAdmissionPolicy {
  /** When true and contact_peer_ids non-empty, refuse non-contact dialers (legacy; see serve_scope_mask). */
  bool prefer_contacts_only = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

/**
 * Why a relay refuses a bridge / reserve request: the `code` of its error ack (docs/ERRORS.md).
 * The ack's `error` text is for logs only.
 */
enum class CircuitRefusal : int32_t {
  Ok = 0,
  /** The target is not parked on this relay (no live link to it). */
  TargetNotReserved,
  /** The relay could not reach the target (dial, channel or its stream failed). */
  TargetUnreachable,
  Busy,
  /** Scope (a stranger), or a dialer the relay cannot name. */
  Refused,
  NotReady,
  BadRequest,
  Generic,
};

inline const char* CircuitRefusalWire(const CircuitRefusal refusal) {
  switch (refusal) {
  case CircuitRefusal::TargetNotReserved:
    return "target_not_reserved";
  case CircuitRefusal::TargetUnreachable:
    return "target_unreachable";
  case CircuitRefusal::Busy:
    return "busy";
  case CircuitRefusal::Refused:
    return "refused";
  case CircuitRefusal::NotReady:
    return "not_ready";
  case CircuitRefusal::BadRequest:
    return "bad_request";
  case CircuitRefusal::Ok:
  case CircuitRefusal::Generic:
    break;
  }
  return "";
}

/**
 * A relay's refusal from its ack. Relays before the `code` field sent text only: that text is read
 * here, and nowhere else.
 */
inline CircuitRefusal CircuitRefusalFromAck(const std::string_view code, const std::string_view legacy_text) {
  for (const CircuitRefusal refusal :
       {CircuitRefusal::TargetNotReserved, CircuitRefusal::TargetUnreachable, CircuitRefusal::Busy,
        CircuitRefusal::Refused, CircuitRefusal::NotReady, CircuitRefusal::BadRequest}) {
    if (!code.empty() && code == CircuitRefusalWire(refusal)) {
      return refusal;
    }
  }
  if (!code.empty()) {
    return CircuitRefusal::Generic;  // a code from a newer relay
  }
  const auto has = [legacy_text](const char* needle) { return legacy_text.find(needle) != std::string_view::npos; };
  if (has("not registered")) {
    return CircuitRefusal::TargetNotReserved;
  }
  if (has("relay busy")) {
    return CircuitRefusal::Busy;
  }
  if (has("stranger") || has("peer id unknown")) {
    return CircuitRefusal::Refused;
  }
  if (has("not ready")) {
    return CircuitRefusal::NotReady;
  }
  if (has("unsupported op") || has("invalid") || has("missing")) {
    return CircuitRefusal::BadRequest;
  }
  if (has("timed out") || has("dial") || has("transport") || has("channel")) {
    return CircuitRefusal::TargetUnreachable;
  }
  return CircuitRefusal::Generic;
}

} // namespace pp::mesh
