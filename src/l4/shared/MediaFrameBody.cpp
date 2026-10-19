#include "domain/mesh/l4/shared/MediaFrameBody.h"

#include "crypto/Aead.h"

#include <cstring>
#include "common/PbrCompat.h"

namespace pbr {

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
  auto encrypted = pp::XChaCha20Poly1305::Seal(media_key, plain, aad, pp::XChaCha20Poly1305::RandomNonce());
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
  if (body.size() < header_bytes + pp::XChaCha20Poly1305::kNonceBytes) {
    return Error("media frame truncated");
  }
  const ByteVector aad(aad_str.begin(), aad_str.end());
  pp::XChaCha20Poly1305::Sealed blob;
  blob.nonce.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes),
                    body.begin() + static_cast<std::ptrdiff_t>(header_bytes + pp::XChaCha20Poly1305::kNonceBytes));
  blob.ciphertext.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes + pp::XChaCha20Poly1305::kNonceBytes), body.end());
  auto decrypted = pp::XChaCha20Poly1305::Open(media_key, blob, aad);
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

} // namespace pbr
