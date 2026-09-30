#pragma once

#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * Per-inbound-stream attach handshake SM (N026 / MEDIA_RELAY_ATTACH.md).
 * HostSession stays a map object; this only sequences quote → accept → attach.
 */
struct MediaRelayAttachSm {
  MediaRelayAttachPhase phase = MediaRelayAttachPhase::Control;
  std::string remote;
  std::string call_id;
  std::string accepted_quote_id;
  std::string session_token;
  /** B009: the video levels agreed in the accepted quote; the participant carries them. */
  std::vector<uint8_t> video_levels;

  void SetPhase(MediaRelayAttachPhase next, MediaRelayAttachEvent ev);
  /** Sole legal attach-phase transition entry for one inbound control stream. */
  bool Apply(MediaRelayAttachEvent ev);
};

} // namespace pbr
