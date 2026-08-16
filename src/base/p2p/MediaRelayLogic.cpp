#include "base/p2p/MediaRelayLogic.h"

namespace pbr {

const char* MediaRelayClientPhaseName(const MediaRelayClientPhase phase) {
  switch (phase) {
  case MediaRelayClientPhase::Idle:
    return "Idle";
  case MediaRelayClientPhase::Dialing:
    return "Dialing";
  case MediaRelayClientPhase::Accepting:
    return "Accepting";
  case MediaRelayClientPhase::Attaching:
    return "Attaching";
  case MediaRelayClientPhase::Attached:
    return "Attached";
  case MediaRelayClientPhase::Detaching:
    return "Detaching";
  }
  return "?";
}

const char* MediaRelayClientEventName(const MediaRelayClientEvent ev) {
  switch (ev) {
  case MediaRelayClientEvent::AttachRequested:
    return "AttachRequested";
  case MediaRelayClientEvent::OpenStreamOk:
    return "OpenStreamOk";
  case MediaRelayClientEvent::OpenStreamFail:
    return "OpenStreamFail";
  case MediaRelayClientEvent::AcceptOk:
    return "AcceptOk";
  case MediaRelayClientEvent::AcceptFail:
    return "AcceptFail";
  case MediaRelayClientEvent::AttachOk:
    return "AttachOk";
  case MediaRelayClientEvent::AttachFail:
    return "AttachFail";
  case MediaRelayClientEvent::DetachRequested:
    return "DetachRequested";
  case MediaRelayClientEvent::AttachTimeout:
    return "AttachTimeout";
  case MediaRelayClientEvent::DuplexLost:
    return "DuplexLost";
  case MediaRelayClientEvent::AttachSuperseded:
    return "AttachSuperseded";
  }
  return "?";
}

MediaRelayClientPhaseOutcome DecideMediaRelayClientPhase(const MediaRelayClientPhase phase,
                                                         const MediaRelayClientEvent ev) {
  MediaRelayClientPhaseOutcome out;
  switch (ev) {
  case MediaRelayClientEvent::AttachRequested:
    out.decision = MediaRelayClientPhaseDecision::Transition;
    out.next = MediaRelayClientPhase::Dialing;
    return out;

  case MediaRelayClientEvent::OpenStreamOk:
    if (phase != MediaRelayClientPhase::Dialing) {
      out.decision = MediaRelayClientPhaseDecision::Ignore;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Transition;
    out.next = MediaRelayClientPhase::Accepting;
    return out;

  case MediaRelayClientEvent::OpenStreamFail:
  case MediaRelayClientEvent::AcceptFail:
  case MediaRelayClientEvent::AttachFail:
  case MediaRelayClientEvent::AttachTimeout:
  case MediaRelayClientEvent::AttachSuperseded:
    if (phase == MediaRelayClientPhase::Dialing || phase == MediaRelayClientPhase::Accepting ||
        phase == MediaRelayClientPhase::Attaching) {
      out.decision = MediaRelayClientPhaseDecision::Transition;
      out.next = MediaRelayClientPhase::Idle;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Ignore;
    return out;

  case MediaRelayClientEvent::AcceptOk:
    if (phase != MediaRelayClientPhase::Accepting) {
      out.decision = MediaRelayClientPhaseDecision::Ignore;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Transition;
    out.next = MediaRelayClientPhase::Attaching;
    return out;

  case MediaRelayClientEvent::AttachOk:
    if (phase != MediaRelayClientPhase::Attaching) {
      out.decision = MediaRelayClientPhaseDecision::Ignore;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Transition;
    out.next = MediaRelayClientPhase::Attached;
    return out;

  case MediaRelayClientEvent::DuplexLost:
    if (phase == MediaRelayClientPhase::Idle || phase == MediaRelayClientPhase::Detaching) {
      out.decision = MediaRelayClientPhaseDecision::Ignore;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Transition;
    out.next = MediaRelayClientPhase::Idle;
    return out;

  case MediaRelayClientEvent::DetachRequested:
    // Detach() drives Detaching → Idle via SetClientPhaseLocked for Attached;
    // abort in-flight Dialing/Accepting/Attaching here.
    if (phase == MediaRelayClientPhase::Dialing || phase == MediaRelayClientPhase::Accepting ||
        phase == MediaRelayClientPhase::Attaching) {
      out.decision = MediaRelayClientPhaseDecision::Transition;
      out.next = MediaRelayClientPhase::Idle;
      return out;
    }
    out.decision = MediaRelayClientPhaseDecision::Keep;
    return out;
  }
  out.decision = MediaRelayClientPhaseDecision::Ignore;
  return out;
}

bool MediaRelayAuthStubOk(const std::string& auth, const std::string& call_id) {
  return !auth.empty() && auth == call_id;
}

bool ShouldDropStaleLossyFrame(const bool has_last, const uint32_t last_seq, const uint32_t seq,
                               const uint8_t mark) {
  return has_last && seq < last_seq && mark == 0;
}

bool MediaRelayCallScopedAdmit(const bool session_exists_for_call,
                               const bool peer_passes_contact_admit) {
  return session_exists_for_call || peer_passes_contact_admit;
}

bool MediaRelayCanOpenHostSession(const size_t current_sessions, const size_t max_sessions) {
  return current_sessions < max_sessions;
}

bool MediaRelayCanAddParticipant(const size_t current_participants, const size_t max_participants) {
  return current_participants < max_participants;
}

} // namespace pbr
