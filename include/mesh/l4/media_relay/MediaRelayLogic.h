#pragma once

#include "mesh/l4/media_relay/MediaRelayTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pp::mesh {

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

/** Pure client-phase table (N026). Logging / SetPhase stay in MediaRelayRuntime. */
MediaRelayClientPhaseOutcome DecideMediaRelayClientPhase(MediaRelayClientPhase phase, MediaRelayClientEvent ev);

/** auth stub: non-empty and equals call_id (v1 dogfood). */
bool MediaRelayAuthStubOk(const std::string& auth, const std::string& call_id);

/**
 * LatestLossy stale drop: seq regresses and mark==0 → drop.
 * `has_last` false means no prior seq for this sub key.
 */
bool ShouldDropStaleLossyFrame(bool has_last, uint32_t last_seq, uint32_t seq, uint8_t mark);

/**
 * Media on a reliable channel — the control channel carrying it while no data lane is open (a hop behind a circuit,
 * one that serves no lane, or a lane not open yet) — waits behind everything that channel already queued for window
 * space, and a reliable channel never drops. Once its oldest queued frame has waited longer than this, a new media
 * frame is dropped instead of queued (V063: call media is best effort; a late frame only delays the ones after it).
 * Audio 150 ms, video 300 ms: a hop adds at most about that to a call already at ~200 ms one-way.
 */
inline constexpr int64_t kMediaFallbackAudioDeadlineMs = 150;
inline constexpr int64_t kMediaFallbackVideoDeadlineMs = 300;
int64_t MediaFallbackDeadlineMs(MediaChannelType channel_type);
bool ShouldDropLateFallbackFrame(int64_t backlog_oldest_age_ms, MediaChannelType channel_type);

/** Call-scoped admit: existing HostSession for call_id admits strangers. */
bool MediaRelayCallScopedAdmit(bool session_exists_for_call, bool peer_passes_contact_admit);

bool MediaRelayCanOpenHostSession(size_t current_sessions, size_t max_sessions);
bool MediaRelayCanAddParticipant(size_t current_participants, size_t max_participants);

} // namespace pp::mesh
