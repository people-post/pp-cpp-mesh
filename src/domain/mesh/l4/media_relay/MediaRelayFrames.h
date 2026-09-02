#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

/** N021 media data frame wire constants (shared by encode/decode + length-prefixed IO). */
inline constexpr uint8_t kMediaDataVersion = 1;
inline constexpr size_t kMediaDataHeaderBytes = 1 + 4 + 2 + 1 + 4 + 1; // ver+stream+chan+type+seq+mark
inline constexpr size_t kMaxMediaFrameBytes = 256 * 1024;

} // namespace pbr
