#pragma once

#include <cstddef>
#include <cstdint>

namespace pp::mesh {

/** The largest media frame a relay takes (its wire is mesh/l4/shared/MediaFrame.h, A029 H2). */
inline constexpr size_t kMaxMediaFrameBytes = 256 * 1024;

/** Subscription key for a (stream, channel) pair — client and server subscription sets. */
inline uint64_t MediaRelaySubKey(const uint32_t stream_id, const uint16_t channel_id) {
  return (static_cast<uint64_t>(stream_id) << 16) | channel_id;
}

} // namespace pp::mesh
