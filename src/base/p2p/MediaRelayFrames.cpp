#include "base/p2p/MediaRelayFrames.h"
#include "base/p2p/MediaRelayService.h"

#include <cstring>

namespace pbr {

std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame) {
  std::vector<uint8_t> body(kMediaDataHeaderBytes + frame.payload.size());
  size_t i = 0;
  body[i++] = kMediaDataVersion;
  auto put_u32 = [&](uint32_t v) {
    body[i++] = static_cast<uint8_t>((v >> 24) & 0xff);
    body[i++] = static_cast<uint8_t>((v >> 16) & 0xff);
    body[i++] = static_cast<uint8_t>((v >> 8) & 0xff);
    body[i++] = static_cast<uint8_t>(v & 0xff);
  };
  auto put_u16 = [&](uint16_t v) {
    body[i++] = static_cast<uint8_t>((v >> 8) & 0xff);
    body[i++] = static_cast<uint8_t>(v & 0xff);
  };
  put_u32(frame.stream_id);
  put_u16(frame.channel_id);
  body[i++] = static_cast<uint8_t>(frame.channel_type);
  put_u32(frame.seq);
  body[i++] = frame.mark;
  if (!frame.payload.empty()) {
    std::memcpy(body.data() + i, frame.payload.data(), frame.payload.size());
  }
  return body;
}

Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body) {
  if (body.size() < kMediaDataHeaderBytes) {
    return Error("media data frame too short");
  }
  if (body[0] != kMediaDataVersion) {
    return Error("unsupported media data version");
  }
  auto get_u32 = [&](size_t at) -> uint32_t {
    return (static_cast<uint32_t>(body[at]) << 24) | (static_cast<uint32_t>(body[at + 1]) << 16) |
           (static_cast<uint32_t>(body[at + 2]) << 8) | static_cast<uint32_t>(body[at + 3]);
  };
  auto get_u16 = [&](size_t at) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(body[at]) << 8) | body[at + 1]);
  };
  MediaDataFrame frame;
  frame.stream_id = get_u32(1);
  frame.channel_id = get_u16(5);
  frame.channel_type = static_cast<MediaChannelType>(body[7]);
  frame.seq = get_u32(8);
  frame.mark = body[12];
  frame.payload.assign(body.begin() + static_cast<std::ptrdiff_t>(kMediaDataHeaderBytes), body.end());
  return frame;
}

} // namespace pbr
