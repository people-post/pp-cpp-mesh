#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/MessageCipher.h"

#include <cstring>
#include "common/PbrCompat.h"

namespace pbr {

std::string BuildMediaRelayFrameAad(const std::string& context, uint32_t media_epoch, uint32_t stream_id,
                                    uint32_t seq, uint8_t channel) {
  return context + "|" + std::to_string(media_epoch) + "|" + std::to_string(stream_id) + "|" +
         std::to_string(seq) + "|" + std::to_string(channel);
}

uint32_t ReadMediaFrameSeq(const std::vector<uint8_t>& body) {
  return (static_cast<uint32_t>(body[1]) << 24) | (static_cast<uint32_t>(body[2]) << 16) |
         (static_cast<uint32_t>(body[3]) << 8) | static_cast<uint32_t>(body[4]);
}

Roe<std::vector<uint8_t>> SealMediaFrameV2Body(const ByteVector& media_key, const std::string& aad_str, uint32_t seq,
                                               uint8_t mark, uint8_t channel, const std::vector<uint8_t>& payload) {
  if (media_key.empty()) {
    return Error("media key required");
  }
  const ByteVector aad(aad_str.begin(), aad_str.end());
  const ByteVector plain(payload.begin(), payload.end());
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  auto encrypted = MessageCipher::Encrypt(media_key, plain, aad, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  std::vector<uint8_t> body(kMediaFrameV2HeaderBytes + encrypted->nonce.size() + encrypted->ciphertext.size());
  size_t i = 0;
  body[i++] = kMediaFrameVersionV2;
  body[i++] = static_cast<uint8_t>((seq >> 24) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 16) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 8) & 0xff);
  body[i++] = static_cast<uint8_t>(seq & 0xff);
  body[i++] = mark;
  body[i++] = channel;
  std::memcpy(body.data() + i, encrypted->nonce.data(), encrypted->nonce.size());
  i += encrypted->nonce.size();
  std::memcpy(body.data() + i, encrypted->ciphertext.data(), encrypted->ciphertext.size());
  return body;
}

Roe<MediaFrameOpened> OpenMediaFrameBody(const ByteVector& media_key, const std::string& aad_str, uint8_t channel,
                                         uint32_t seq, uint8_t mark, size_t header_bytes,
                                         const std::vector<uint8_t>& body) {
  if (media_key.empty()) {
    return Error("media key required");
  }
  if (body.size() < header_bytes + kAeadNonceSize) {
    return Error("media frame truncated");
  }
  const ByteVector aad(aad_str.begin(), aad_str.end());
  EncryptedBlob blob;
  blob.nonce.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes),
                    body.begin() + static_cast<std::ptrdiff_t>(header_bytes + kAeadNonceSize));
  blob.ciphertext.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes + kAeadNonceSize), body.end());
  auto decrypted = MessageCipher::Decrypt(media_key, blob, aad);
  if (!decrypted) {
    return decrypted.error();
  }
  MediaFrameOpened out;
  out.channel = channel;
  out.seq = seq;
  out.mark = mark;
  out.payload.assign(decrypted->begin(), decrypted->end());
  return out;
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
