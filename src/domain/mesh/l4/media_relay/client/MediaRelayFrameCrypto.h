#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

using ByteVector = std::vector<uint8_t>;

/** Frame body version with a channel byte: `[2][seq u32 BE][mark][channel][nonce][ciphertext]`. */
inline constexpr uint8_t kMediaFrameVersionV2 = 2;
inline constexpr size_t kMediaFrameV2HeaderBytes = 1 + 4 + 1 + 1;

struct MediaFrameOpened {
  uint8_t channel = 0;
  uint32_t seq = 0;
  uint8_t mark = 0;
  std::vector<uint8_t> payload;
};

/**
 * AEAD AAD for an end-to-end encrypted `media_relay` frame: `<context>|epoch|stream_id|seq|channel`.
 * `context` is the owning feature's label + session (media-client-layers L006): calls use
 * `call-media-sfu|<call_id>`, broadcast `broadcast-media|<program_id>|<join_handle>`, so a key or
 * frame from one feature / session never opens under another. Relays forward opaque bodies.
 */
std::string BuildMediaRelayFrameAad(const std::string& context, uint32_t media_epoch, uint32_t stream_id,
                                    uint32_t seq, uint8_t channel);

Roe<std::vector<uint8_t>> SealMediaRelayFrame(const ByteVector& media_key, const std::string& context,
                                              uint32_t media_epoch, uint32_t stream_id, uint32_t seq, uint8_t mark,
                                              uint8_t channel, const std::vector<uint8_t>& payload);

/** v2 bodies only; `channel` is the relay channel the frame arrived on (must match the body). */
Roe<MediaFrameOpened> OpenMediaRelayFrame(const ByteVector& media_key, const std::string& context,
                                          uint32_t media_epoch, uint32_t stream_id, uint8_t channel,
                                          const std::vector<uint8_t>& body);

// --- Body primitives (AAD supplied) — shared with call_media's direct / legacy framings. ---

uint32_t ReadMediaFrameSeq(const std::vector<uint8_t>& body);
Roe<std::vector<uint8_t>> SealMediaFrameV2Body(const ByteVector& media_key, const std::string& aad, uint32_t seq,
                                               uint8_t mark, uint8_t channel, const std::vector<uint8_t>& payload);
/** Opens `[header_bytes][nonce][ciphertext]`; header fields were parsed by the caller. */
Roe<MediaFrameOpened> OpenMediaFrameBody(const ByteVector& media_key, const std::string& aad, uint8_t channel,
                                         uint32_t seq, uint8_t mark, size_t header_bytes,
                                         const std::vector<uint8_t>& body);

} // namespace pbr
