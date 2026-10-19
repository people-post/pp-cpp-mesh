#pragma once

#include "domain/mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * H011 Circuit R1 rendezvous — answerer park coverage for the shared surface.
 * Dialer StartBridge budget (H010) and answerer park depth share the same K.
 */
inline constexpr std::size_t kCircuitRendezvousParkCoverage = kCircuitMaxStartBridgeAttempts;

/**
 * Answerer park order over the shared rendezvous surface (H011 L3.1a/b):
 * optional sticky last-good R1 first, then Connected, then the rest (stable within bands).
 * Same banding as dialer `OrderCircuitRelayAttempts` so park covers dialer sticky reorder.
 */
inline std::vector<std::string> OrderRendezvousParkAttempts(
    std::vector<std::string> surface, const std::string& sticky,
    const std::function<bool(const std::string&)>& is_connected) {
  return OrderCircuitRelayAttempts(std::move(surface), sticky, is_connected);
}

/** What a bootstrap-seed park wait does on each check (H010; call-path-resilience k2). */
enum class SeedParkStep {
  Wait,        // keep waiting (listener / timers)
  ArmGrace,    // ≥1 seed Connected: give the others a short grace instead of the whole deadline
  SettleOk,    // parked
  SettleFail,  // deadline with no Connected seed
};

/**
 * All seeds Connected settles at once (the dialer may bridge through any seed). With some but not
 * all, a short grace starts once; at the deadline (or the grace's end) ≥1 Connected is enough.
 */
inline SeedParkStep DecideSeedParkStep(const bool all_connected, const bool any_connected, const bool at_deadline,
                                       const bool grace_armed) {
  if (all_connected) {
    return SeedParkStep::SettleOk;
  }
  if (at_deadline) {
    return any_connected ? SeedParkStep::SettleOk : SeedParkStep::SettleFail;
  }
  if (any_connected && !grace_armed) {
    return SeedParkStep::ArmGrace;
  }
  return SeedParkStep::Wait;
}

/** Overload without sticky (Connected-first only). */
inline std::vector<std::string> OrderRendezvousParkAttempts(
    std::vector<std::string> surface,
    const std::function<bool(const std::string&)>& is_connected) {
  return OrderRendezvousParkAttempts(std::move(surface), {}, is_connected);
}

/**
 * How many non-Connected surface members to cold-dial+reserve after Connected are covered.
 *
 * - Always reserve every Connected member (caller responsibility).
 * - Default product: `cover_all_remaining=true` — serial cold-dial the rest (dual-NAT
 *   dialer may sticky-reorder any surface member to try #1).
 * - When `cover_all_remaining=false`: cold-dial only enough to reach `coverage_k` total
 *   members (Connected + cold), matching H011 minimum coverage.
 */
inline std::size_t RendezvousColdDialLimit(const std::size_t surface_size,
                                           const std::size_t connected_count,
                                           const std::size_t coverage_k,
                                           const bool cover_all_remaining) {
  if (surface_size == 0) {
    return 0;
  }
  const std::size_t connected = std::min(connected_count, surface_size);
  const std::size_t remaining = surface_size - connected;
  if (cover_all_remaining) {
    return remaining;
  }
  if (connected >= coverage_k) {
    return 0;
  }
  return std::min(remaining, coverage_k - connected);
}

/** True when park coverage meets H011 minimum (Connected + reserved/cold attempts). */
inline bool RendezvousParkCoverageMet(const std::size_t covered_members,
                                      const std::size_t surface_size,
                                      const std::size_t coverage_k) {
  if (surface_size == 0) {
    return false;
  }
  const std::size_t need = std::min(coverage_k, surface_size);
  return covered_members >= need;
}

} // namespace pbr
