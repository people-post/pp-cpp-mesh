#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

using ByteVector = std::vector<uint8_t>;

/**
 * End-to-end media frame bodies, shared by media_relay and call_media (each supplies its own AAD).
 * Relays forward these opaque.
 */

/** Frame body version with a channel byte: `[2][seq u32 BE][mark][channel][nonce][ciphertext]`. */
inline constexpr uint8_t kMediaFrameVersionV2 = 2;
inline constexpr size_t kMediaFrameV2HeaderBytes = 1 + 4 + 1 + 1;

struct MediaFrameOpened {
  uint8_t channel = 0;
  uint32_t seq = 0;
  uint8_t mark = 0;
  std::vector<uint8_t> payload;
};

uint32_t ReadMediaFrameSeq(const std::vector<uint8_t>& body);
Roe<std::vector<uint8_t>> SealMediaFrameV2Body(const ByteVector& media_key, const std::string& aad, uint32_t seq,
                                               uint8_t mark, uint8_t channel, const std::vector<uint8_t>& payload);
/** Opens `[header_bytes][nonce][ciphertext]`; header fields were parsed by the caller. */
Roe<MediaFrameOpened> OpenMediaFrameBody(const ByteVector& media_key, const std::string& aad, uint8_t channel,
                                         uint32_t seq, uint8_t mark, size_t header_bytes,
                                         const std::vector<uint8_t>& body);

} // namespace pbr
