#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"

#include "common/PbrCompat.h"

namespace pbr {

std::string BuildMediaRelayFrameAad(const std::string& context, uint32_t media_epoch, uint32_t stream_id,
                                    uint32_t seq, uint8_t channel) {
  return context + "|" + std::to_string(media_epoch) + "|" + std::to_string(stream_id) + "|" +
         std::to_string(seq) + "|" + std::to_string(channel);
}

Roe<std::vector<uint8_t>> SealMediaRelayFrame(const ByteVector& media_key, const std::string& context,
                                              uint32_t media_epoch, uint32_t stream_id, uint32_t seq, uint8_t mark,
                                              uint8_t channel, const std::vector<uint8_t>& payload) {
  return SealMediaFrameV2Body(media_key, BuildMediaRelayFrameAad(context, media_epoch, stream_id, seq, channel), seq,
                              mark, channel, payload);
}

Roe<MediaFrameOpened> OpenMediaRelayFrame(const ByteVector& media_key, const std::string& context,
                                          uint32_t media_epoch, uint32_t stream_id, uint8_t channel,
                                          const std::vector<uint8_t>& body) {
  if (body.size() < kMediaFrameV2HeaderBytes || body[0] != kMediaFrameVersionV2) {
    return Error("unsupported media frame version");
  }
  if (body[6] != channel) {
    return Error("media frame channel mismatch");
  }
  const uint32_t seq = ReadMediaFrameSeq(body);
  return OpenMediaFrameBody(media_key, BuildMediaRelayFrameAad(context, media_epoch, stream_id, seq, channel),
                            channel, seq, body[5], kMediaFrameV2HeaderBytes, body);
}

} // namespace pbr
