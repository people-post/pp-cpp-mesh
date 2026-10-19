#pragma once

#include "common/Error.h"
#include "domain/mesh/l4/shared/MediaFrameBody.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

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

} // namespace pbr
