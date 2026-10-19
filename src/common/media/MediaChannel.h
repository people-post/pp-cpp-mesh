#pragma once

#include <cstdint>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A media frame's channel: track kind in the high nibble, level in the low nibble
 * (docs/contracts/MEDIA_CHANNELS.md). Audio has no level; video levels are opaque ordered
 * integers (higher = more bits) whose meaning is the sending client's.
 */
enum class MediaTrackKind : uint8_t { Audio = 0, Video = 1 };

inline constexpr uint8_t kMediaChannelAudio = 0x00;
inline constexpr uint8_t kMinVideoLevel = 1;
inline constexpr uint8_t kMaxVideoLevel = 15;
/** The single level a client sends when it sends one (calls; a phone's broadcast). */
inline constexpr uint8_t kDefaultVideoLevel = 1;

constexpr bool IsVideoLevel(const int level) {
  return level >= kMinVideoLevel && level <= kMaxVideoLevel;
}

/** The channel of video at `level` (a level outside 1–15 is not a video channel). */
constexpr uint8_t VideoChannel(const uint8_t level) {
  return static_cast<uint8_t>((static_cast<uint8_t>(MediaTrackKind::Video) << 4) | (level & 0x0F));
}

constexpr bool IsAudioChannel(const uint16_t channel) {
  return channel == kMediaChannelAudio;
}

constexpr bool IsVideoChannel(const uint16_t channel) {
  return channel <= 0xFF && (channel >> 4) == static_cast<uint8_t>(MediaTrackKind::Video) &&
         IsVideoLevel(channel & 0x0F);
}

/** The level of a video channel; 0 for any other channel. */
constexpr uint8_t VideoLevelOf(const uint16_t channel) {
  return IsVideoChannel(channel) ? static_cast<uint8_t>(channel & 0x0F) : 0;
}

} // namespace pbr
