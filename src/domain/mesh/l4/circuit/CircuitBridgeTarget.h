#pragma once

#include <string>

namespace pbr {

/** Client/relay bridge destination (media-hop L3 / Amp circuit). */
/**
 * A circuit that backs up a call's direct path (call-path-resilience K003). A loaded relay refuses
 * standby circuits lowest priority first; None = a primary circuit (never refused for load).
 */
enum class CircuitStandbyPriority {
  None = 0,
  Low,
  Medium,
  High,
};

/** Wire value of the bridge request's optional `standby_priority`; None → nullptr (field omitted). */
inline const char* CircuitStandbyPriorityWire(const CircuitStandbyPriority priority) {
  switch (priority) {
  case CircuitStandbyPriority::Low:
    return "low";
  case CircuitStandbyPriority::Medium:
    return "medium";
  case CircuitStandbyPriority::High:
    return "high";
  case CircuitStandbyPriority::None:
    break;
  }
  return nullptr;
}

/** Missing → None; an unknown value from a newer peer → Low (still a standby, refused first). */
inline CircuitStandbyPriority ParseCircuitStandbyPriority(const std::string& wire) {
  if (wire.empty()) {
    return CircuitStandbyPriority::None;
  }
  if (wire == "high") {
    return CircuitStandbyPriority::High;
  }
  if (wire == "medium") {
    return CircuitStandbyPriority::Medium;
  }
  return CircuitStandbyPriority::Low;
}

struct CircuitBridgeTarget {
  /** Target PeerId base58; required when multiaddr empty. */
  std::string target_peer_id;
  /** Optional explicit multiaddr; relay may still resolve peer_id when empty. */
  std::string target_multiaddr;
  /** Stream protocol on target after bridge (default circuit-relay). */
  std::string target_protocol;
  /** A call's standby circuit (K003); None for a primary one. */
  CircuitStandbyPriority standby_priority = CircuitStandbyPriority::None;
};

} // namespace pbr
