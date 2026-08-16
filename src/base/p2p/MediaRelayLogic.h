#pragma once

#include "base/p2p/MediaRelayService.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/** Result of applying a client attach event to the current phase (pure). */
enum class MediaRelayClientPhaseDecision {
  /** Transition to `next`. */
  Transition = 0,
  /** Event accepted but phase unchanged (e.g. DetachRequested while Attached). */
  Keep,
  /** Illegal / stale for this phase — ignore. */
  Ignore,
};

struct MediaRelayClientPhaseOutcome {
  MediaRelayClientPhaseDecision decision = MediaRelayClientPhaseDecision::Ignore;
  MediaRelayClientPhase next = MediaRelayClientPhase::Idle;
};

/** Pure client-phase table (N026). Logging / SetPhase stay in MediaRelayService::Impl. */
MediaRelayClientPhaseOutcome DecideMediaRelayClientPhase(MediaRelayClientPhase phase,
                                                         MediaRelayClientEvent ev);

/** auth stub: non-empty and equals call_id (v1 dogfood). */
bool MediaRelayAuthStubOk(const std::string& auth, const std::string& call_id);

/**
 * LatestLossy stale drop: seq regresses and mark==0 → drop.
 * `has_last` false means no prior seq for this sub key.
 */
bool ShouldDropStaleLossyFrame(bool has_last, uint32_t last_seq, uint32_t seq, uint8_t mark);

/** Call-scoped admit: existing HostSession for call_id admits strangers. */
bool MediaRelayCallScopedAdmit(bool session_exists_for_call, bool peer_passes_contact_admit);

bool MediaRelayCanOpenHostSession(size_t current_sessions, size_t max_sessions);
bool MediaRelayCanAddParticipant(size_t current_participants, size_t max_participants);

} // namespace pbr
