#pragma once

#include "mesh/types/RelayScope.h"
#include "mesh/l4/shared/L4ProtocolIds.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pp::mesh {

/** Provider admission (nf / N023): scope mask + contact PeerIds. */
struct CircuitRelayAdmissionPolicy {
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

/** Every refusal a relay can send, Generic last (metrics label each; `CircuitRefusalWire` names them). */
inline constexpr std::array<CircuitRefusal, 7> kCircuitRefusalReasons{
    CircuitRefusal::TargetNotReserved, CircuitRefusal::TargetUnreachable, CircuitRefusal::Busy,
    CircuitRefusal::Refused,           CircuitRefusal::NotReady,          CircuitRefusal::BadRequest,
    CircuitRefusal::Generic,
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

/** A relay's refusal from its ack's `code`; a missing or unknown code (a newer relay's) is a plain failure. */
inline CircuitRefusal CircuitRefusalFromAck(const std::string_view code) {
  for (const CircuitRefusal refusal : kCircuitRefusalReasons) {
    if (refusal != CircuitRefusal::Generic && code == CircuitRefusalWire(refusal)) {
      return refusal;
    }
  }
  return CircuitRefusal::Generic;
}

} // namespace pp::mesh
