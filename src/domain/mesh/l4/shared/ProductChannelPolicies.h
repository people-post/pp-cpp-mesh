#pragma once

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"

#include <chrono>

namespace pp::amp {

/** Realtime media frames for `/pp-browser/realtime/1.0.0`. */
inline ChannelPolicy CallMediaChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = AmpChannelLimits::kMaxCallMediaOutboundFrames;
  policy.write_preferred = true;
  policy.max_message_bytes = AmpChannelLimits::kMaxCallMediaFrameBytes;
  return policy;
}

/** Reliable hello/teardown leg for `/pp-browser/realtime/1.0.0` (AMP-CHANNEL). */
inline ChannelPolicy CallMediaControlChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::RealtimeControl;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = std::chrono::milliseconds{8000};
  return policy;
}

/**
 * Product Bulk OPEN for `/pp-browser/blob/1.0.0`.
 * Wraps Amp `MakeBulkChannelPolicy()` (class + size) and adds read_once / timeout.
 * Do not re-copy Bulk defaults here — see pp-cpp-amp docs/OWNERSHIP.md.
 */
inline ChannelPolicy BulkChannelPolicy(bool read_once) {
  // Call MakeBulkChannelPolicy (not BulkChannelPolicy()) to avoid overload recursion
  // with this (bool) product wrapper.
  ChannelPolicy policy = MakeBulkChannelPolicy();
  policy.read_once = read_once;
  policy.read_timeout = std::chrono::milliseconds{8000};
  return policy;
}

/**
 * `/pp-browser/circuit/1.0.0` tunnel: JSON bridge handshake then opaque DATA splice.
 * Reliable; not read_once (stays open for forward).
 */
inline ChannelPolicy CircuitTunnelChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
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

/** Media-relay client attach leg — Reliable control + attach JSON. */
inline ChannelPolicy MediaRelayClientChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
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
