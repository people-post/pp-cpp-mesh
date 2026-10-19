#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

/** N021 media data frame wire constants (shared by encode/decode + length-prefixed IO). */
inline constexpr uint8_t kMediaDataVersion = 1;
inline constexpr size_t kMediaDataHeaderBytes = 1 + 4 + 2 + 1 + 4 + 1; // ver+stream+chan+type+seq+mark
inline constexpr size_t kMaxMediaFrameBytes = 256 * 1024;

/** Subscription key for a (stream, channel) pair — client and server subscription sets. */
inline uint64_t MediaRelaySubKey(const uint32_t stream_id, const uint16_t channel_id) {
  return (static_cast<uint64_t>(stream_id) << 16) | channel_id;
}

} // namespace pbr
