#pragma once

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"

#include <chrono>

namespace pp::amp {

// Realtime media frames for `/pp/realtime/1.0.0`: amp's core
// CallMediaChannelPolicy() (no read timeout: muted or paused media is silent).

/**
 * Reliable hello/heartbeat/teardown leg for `/pp/realtime/1.0.0` (AMP-CHANNEL).
 * Lives for the call; no read timeout: CallMediaLegCoordinator owns liveness (connect deadline,
  * active-path silence failover, standby staleness), and a standby path's heartbeat is slower
 * than any short channel timeout. Keeps its link hot while open (call-path-resilience K008): a standby
 * path carries nothing until a failover, and a cold link dies after 5 s of silence.
 */
inline ChannelPolicy CallMediaControlChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::RealtimeControl;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  policy.keepalive = KeepaliveTier::Hot;
  return policy;
}

/**
 * Product Bulk OPEN for `/pp/blob/1.0.0`.
 * Wraps Amp `MakeBulkChannelPolicy()` (class + size) and adds read_once / timeout.
 * Do not re-copy Bulk defaults here — see pp-cpp-amp docs/OWNERSHIP.md.
 */
inline ChannelPolicy BulkChannelPolicy(bool read_once) {
  // Call MakeBulkChannelPolicy (not BulkChannelPolicy()) to avoid overload recursion
  // with this (bool) product wrapper. Amp v0.1.6+ names the core factory
  // MakeBulkChannelPolicy (ChatBlobChannelPolicy was removed).
  ChannelPolicy policy = MakeBulkChannelPolicy();
  policy.read_once = read_once;
  policy.read_timeout = std::chrono::milliseconds{8000};
  return policy;
}

/**
 * `/pp-mesh/circuit/1.0.0` tunnel: JSON bridge handshake then opaque DATA splice.
 * Reliable; not read_once (stays open for forward). No read timeout by default: a splice
 * (e.g. one-way relayed media) or a reservation is legitimately silent on one side; the
 * circuit coordinators bound the handshake and the reservation TTL themselves.
 */
inline ChannelPolicy CircuitTunnelChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{0}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

/** Media-relay hop leg — BestEffort realtime fan-out (AMP-CHANNEL; D7b). */
inline ChannelPolicy MediaRelayHopChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = AmpChannelLimits::kMaxMediaRelayOutboundFrames;
  policy.write_preferred = true;
  policy.max_message_bytes = AmpChannelLimits::kMaxMediaDataFrameBytes;
  return policy;
}

/**
 * Media-relay client attach leg — Reliable control + attach JSON, then the session's media.
 * No read timeout by default: a publisher receives nothing back (fan-out skips the sender) and
 * a subscriber's hop may be silent; the media-relay coordinators bound quote/attach themselves.
 */
/**
 * A media-relay data lane (kMediaRelayDataProtocolId): media frames only, best
 * effort, the oldest dropped under a burst. A lost packet costs its frame — no
 * retransmit, no acknowledgements, no head-of-line wait for the frames behind
 * it (audio and video shared one reliable channel with the control: a phone's
 * thin uplink filled with the acks and resends, and a loss stalled every
 * stream).
 */
inline ChannelPolicy MediaRelayDataChannelPolicy() {
  ChannelPolicy policy = CallMediaChannelPolicy();
  policy.max_message_bytes = AmpChannelLimits::kMaxMediaDataFrameBytes;
  return policy;
}

inline ChannelPolicy MediaRelayClientChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{0}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::RealtimeControl;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxMediaRelayClientOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

} // namespace pp::amp
