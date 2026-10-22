#pragma once

#include "mesh/l4/circuit/CircuitHopAttemptBudget.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pp::mesh {

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
  ArmGrace,    // ≥1 relay Connected: give the others a short grace instead of the whole deadline
  SettleOk,    // parked
  SettleFail,  // deadline with no Connected relay
};

/**
 * Enough relays Connected (SeedParkNeeds: the target, or every usable one when fewer) settles at
 * once — the dialer may bridge through any of them. With fewer, a short grace starts once; at the
 * deadline (or the grace's end) ≥1 Connected is enough.
 */
inline SeedParkStep DecideSeedParkStep(const bool enough_connected, const bool any_connected, const bool at_deadline,
                                       const bool grace_armed) {
  if (enough_connected) {
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

/**
 * A seed dial missed while a park waits (its first handshake lost, a seed busy for a moment): dial
 * that seed again rather than sit out the rest of the deadline — while enough of it is left for a
 * handshake, and a bounded number of times per park. Before, one lost dial cost the whole 12 s
 * (the answerer's call_accept waits for the park).
 */
inline constexpr int64_t kSeedParkRedialMinLeftMs = 3000;
inline constexpr int kSeedParkMaxRedials = 2;

inline bool ShouldRedialSeedForPark(const int64_t park_left_ms, const int redials_done) {
  return park_left_ms >= kSeedParkRedialMinLeftMs && redials_done < kSeedParkMaxRedials;
}

/**
 * After a seed dial missed: the park is answered `false` now when nothing can still land it — no
 * seed Connected, no dial of ours in flight, and no redial left to try. Seeds that fail fast
 * (refused, unreachable) then no longer hold an answer for the whole deadline.
 */
inline bool SeedParkCannotLand(const bool any_seed_connected, const int seed_dials_in_flight, const bool redial_left) {
  return !any_seed_connected && seed_dials_in_flight == 0 && !redial_left;
}

/** Overload without sticky (Connected-first only). */
inline std::vector<std::string> OrderRendezvousParkAttempts(
    std::vector<std::string> surface,
    const std::function<bool(const std::string&)>& is_connected) {
  return OrderRendezvousParkAttempts(std::move(surface), {}, is_connected);
}

} // namespace pp::mesh
