#include "base/p2p/MediaRelayAttachSm.h"

#include "common/Logger.h"

namespace pbr {

namespace {

auto AttachLog() {
  return logging::getLogger("MediaRelayService");
}

} // namespace

const char* MediaRelayAttachPhaseName(const MediaRelayAttachPhase phase) {
  switch (phase) {
  case MediaRelayAttachPhase::Control:
    return "Control";
  case MediaRelayAttachPhase::Quoted:
    return "Quoted";
  case MediaRelayAttachPhase::Accepted:
    return "Accepted";
  case MediaRelayAttachPhase::Attaching:
    return "Attaching";
  case MediaRelayAttachPhase::Attached:
    return "Attached";
  case MediaRelayAttachPhase::Rejected:
    return "Rejected";
  case MediaRelayAttachPhase::Closed:
    return "Closed";
  }
  return "?";
}

const char* MediaRelayAttachEventName(const MediaRelayAttachEvent ev) {
  switch (ev) {
  case MediaRelayAttachEvent::StreamOpened:
    return "StreamOpened";
  case MediaRelayAttachEvent::OpQuote:
    return "OpQuote";
  case MediaRelayAttachEvent::OpAccept:
    return "OpAccept";
  case MediaRelayAttachEvent::OpAttach:
    return "OpAttach";
  case MediaRelayAttachEvent::OpUnsupported:
    return "OpUnsupported";
  case MediaRelayAttachEvent::AdmitFail:
    return "AdmitFail";
  case MediaRelayAttachEvent::AttachOk:
    return "AttachOk";
  case MediaRelayAttachEvent::AttachFail:
    return "AttachFail";
  case MediaRelayAttachEvent::Cancel:
    return "Cancel";
  }
  return "?";
}

void MediaRelayAttachSm::SetPhase(const MediaRelayAttachPhase next, const MediaRelayAttachEvent ev) {
  const MediaRelayAttachPhase prev = phase;
  phase = next;
  if (prev == next) {
    AttachLog().info << "media_relay_attach phase=" << MediaRelayAttachPhaseName(prev)
                     << " event=" << MediaRelayAttachEventName(ev) << " call_id=" << call_id
                     << " peer=" << remote;
    return;
  }
  AttachLog().info << "media_relay_attach phase=" << MediaRelayAttachPhaseName(prev) << "->"
                   << MediaRelayAttachPhaseName(next) << " event=" << MediaRelayAttachEventName(ev)
                   << " call_id=" << call_id << " peer=" << remote;
}

bool MediaRelayAttachSm::Apply(const MediaRelayAttachEvent ev) {
  switch (ev) {
  case MediaRelayAttachEvent::StreamOpened:
    SetPhase(MediaRelayAttachPhase::Control, ev);
    return true;
  case MediaRelayAttachEvent::OpQuote:
    if (phase != MediaRelayAttachPhase::Control && phase != MediaRelayAttachPhase::Quoted) {
      return false;
    }
    SetPhase(MediaRelayAttachPhase::Quoted, ev);
    return true;
  case MediaRelayAttachEvent::OpAccept:
    if (phase != MediaRelayAttachPhase::Control && phase != MediaRelayAttachPhase::Quoted) {
      return false;
    }
    SetPhase(MediaRelayAttachPhase::Accepted, ev);
    return true;
  case MediaRelayAttachEvent::OpAttach:
    if (phase != MediaRelayAttachPhase::Control && phase != MediaRelayAttachPhase::Quoted &&
        phase != MediaRelayAttachPhase::Accepted) {
      return false;
    }
    SetPhase(MediaRelayAttachPhase::Attaching, ev);
    return true;
  case MediaRelayAttachEvent::AttachOk:
    if (phase != MediaRelayAttachPhase::Attaching) {
      return false;
    }
    SetPhase(MediaRelayAttachPhase::Attached, ev);
    return true;
  case MediaRelayAttachEvent::AdmitFail:
  case MediaRelayAttachEvent::AttachFail:
  case MediaRelayAttachEvent::OpUnsupported:
    SetPhase(MediaRelayAttachPhase::Rejected, ev);
    SetPhase(MediaRelayAttachPhase::Closed, ev);
    return true;
  case MediaRelayAttachEvent::Cancel:
    SetPhase(MediaRelayAttachPhase::Closed, ev);
    return true;
  }
  return false;
}

} // namespace pbr
