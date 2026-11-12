#include "mesh/l4/shared/MediaFrame.h"

#include "crypto/Aead.h"
#include "crypto/SodiumUtil.h"
#include "mesh/types/MediaChannel.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace pp::mesh {
namespace {

void PutU32(uint8_t* out, const uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>(v >> (8 * (3 - i)));
  }
}

void PutU64(uint8_t* out, const uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
  }
}

uint32_t GetU32(const uint8_t* in) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

uint64_t GetU64(const uint8_t* in) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

/** The header then `tail`, into a buffer sized once (growing one from the 18 B header trips GCC 12's -Warray-bounds). */
template <typename Tail>
ByteVector HeaderThen(const MediaDataFrame& frame, const Tail& tail) {
  const auto header = EncodeMediaFrameHeader(frame);
  ByteVector out(header.size() + tail.size());
  std::copy(header.begin(), header.end(), out.begin());
  std::copy(tail.begin(), tail.end(), out.begin() + static_cast<std::ptrdiff_t>(header.size()));
  return out;
}

ByteVector Aad(const MediaDataFrame& frame, const std::string& context) {
  return HeaderThen(frame, context);
}

/** The guard keeps this many (instance, epoch, channel) entries before it forgets other instances / epochs. */
constexpr size_t kMaxGuardEntries = 64;

} // namespace

MediaChannelType MediaChannelTypeFor(const uint16_t channel_id) {
  return IsAudioChannel(channel_id) ? MediaChannelType::Forward : MediaChannelType::LatestLossy;
}

std::array<uint8_t, kMediaFrameHeaderBytes> EncodeMediaFrameHeader(const MediaDataFrame& frame) {
  std::array<uint8_t, kMediaFrameHeaderBytes> out{};
  out[0] = static_cast<uint8_t>((kMediaFrameVersion << 4) | ((static_cast<uint8_t>(frame.channel_type) & 0x03) << 2) |
                                (frame.mark & 0x01));
  out[1] = static_cast<uint8_t>(frame.channel_id);
  PutU32(out.data() + 2, frame.stream_id);
  PutU32(out.data() + 6, frame.seq);
  PutU64(out.data() + 10, frame.instance);
  return out;
}

std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame) {
  return HeaderThen(frame, frame.payload);
}

Roe<MediaDataFrame> DecodeMediaDataFrameHeader(const std::span<const uint8_t> body) {
  if (body.size() < kMediaFrameHeaderBytes) {
    return Error("media frame too short");
  }
  if ((body[0] >> 4) != kMediaFrameVersion) {
    return Error("unsupported media frame version");
  }
  const uint8_t type = (body[0] >> 2) & 0x03;
  if (type > static_cast<uint8_t>(MediaChannelType::LatestLossy)) {
    return Error("bad media channel type");
  }
  MediaDataFrame frame;
  frame.channel_type = static_cast<MediaChannelType>(type);
  frame.mark = body[0] & 0x01;
  frame.channel_id = body[1];
  frame.stream_id = GetU32(body.data() + 2);
  frame.seq = GetU32(body.data() + 6);
  frame.instance = GetU64(body.data() + 10);
  return frame;
}

Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body) {
  auto frame = DecodeMediaDataFrameHeader(body);
  if (frame) {
    frame->payload.assign(body.begin() + static_cast<std::ptrdiff_t>(kMediaFrameHeaderBytes), body.end());
  }
  return frame;
}

uint64_t NewMediaSenderInstance() {
  pp::EnsureSodiumInit();
  uint64_t v = 0;
  while (v == 0) {
    randombytes_buf(&v, sizeof(v));
  }
  return v;
}

std::array<uint8_t, kMediaFrameNonceBytes> MediaFrameNonce(const uint8_t channel, const uint32_t stream_id,
                                                           const uint64_t instance, const uint32_t seq,
                                                           const uint32_t media_epoch) {
  std::array<uint8_t, kMediaFrameNonceBytes> nonce{};
  nonce[0] = channel;
  PutU32(nonce.data() + 1, stream_id);
  PutU64(nonce.data() + 5, instance);
  PutU32(nonce.data() + 13, seq);
  PutU32(nonce.data() + 17, media_epoch);
  return nonce;
}

Roe<void> SealMediaFrame(const ByteVector& media_key, const std::string& context, const uint32_t media_epoch,
                         MediaDataFrame& frame, const std::vector<uint8_t>& plaintext) {
  if (media_key.empty()) {
    return Error("media key required");
  }
  if (frame.channel_id > 0xFF) {
    return Error("media channel out of range");
  }
  if (frame.instance == 0) {
    return Error("media frame without a sender instance");
  }
  const auto nonce =
      MediaFrameNonce(static_cast<uint8_t>(frame.channel_id), frame.stream_id, frame.instance, frame.seq, media_epoch);
  auto sealed =
      pp::XChaCha20Poly1305::Seal(media_key, plaintext, Aad(frame, context), ByteVector(nonce.begin(), nonce.end()));
  if (!sealed) {
    return sealed.error();
  }
  frame.payload = std::move(sealed->ciphertext);
  return Roe<void>();
}

Roe<std::vector<uint8_t>> OpenMediaFrame(const ByteVector& media_key, const std::string& context,
                                         const uint32_t media_epoch, const MediaDataFrame& frame) {
  if (media_key.empty()) {
    return Error("media key required");
  }
  if (frame.payload.size() < kMediaFrameTagBytes) {
    return Error("media frame truncated");
  }
  const auto nonce =
      MediaFrameNonce(static_cast<uint8_t>(frame.channel_id), frame.stream_id, frame.instance, frame.seq, media_epoch);
  pp::XChaCha20Poly1305::Sealed sealed;
  sealed.nonce.assign(nonce.begin(), nonce.end());
  sealed.ciphertext = frame.payload;
  return pp::XChaCha20Poly1305::Open(media_key, sealed, Aad(frame, context));
}

bool MediaFrameSendGuard::Admit(const uint64_t instance, const uint32_t media_epoch, const uint8_t channel,
                                const uint32_t seq) {
  std::lock_guard lock(mu_);
  const auto key = std::make_tuple(instance, media_epoch, channel);
  auto it = last_.find(key);
  if (it != last_.end() && seq <= it->second) {
    return false;
  }
  if (it == last_.end() && last_.size() >= kMaxGuardEntries) {
    for (auto e = last_.begin(); e != last_.end();) {
      if (std::get<0>(e->first) != instance || std::get<1>(e->first) != media_epoch) {
        e = last_.erase(e);
      } else {
        ++e;
      }
    }
  }
  last_[key] = seq;
  return true;
}

} // namespace pp::mesh
