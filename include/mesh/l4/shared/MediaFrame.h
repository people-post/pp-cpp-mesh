#pragma once

#include "common/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace pp::mesh {

using ByteVector = std::vector<uint8_t>;

/**
 * A media frame (A029 H2): one header for every path a frame takes — through a relay hop (calls, broadcast) or
 * straight to the peer (a 1:1 call) — and one end-to-end seal.
 *
 *   [version (high nibble) | channel type << 2 | mark][channel u8][stream u32][seq u32][instance u64]  (big endian)
 *   [payload sealed with the media key][tag 16]
 *
 * The header travels in the clear (a hop routes by stream and channel, drops stale lossy video by type, seq and
 * mark) and is the seal's associated data, with the owning feature's context. The nonce is derived from it, never
 * sent: channel, stream, instance, seq, key epoch. `instance` is random per sender session (the media engine picks it
 * where its counters start), so a sender that restarts under the same key, or two senders with one stream id, never
 * repeat a nonce. A sender keeps every (channel, seq) once per instance and epoch (MediaFrameSendGuard).
 */
inline constexpr uint8_t kMediaFrameVersion = 3;
inline constexpr size_t kMediaFrameHeaderBytes = 1 + 1 + 4 + 4 + 8;
inline constexpr size_t kMediaFrameTagBytes = 16;
inline constexpr size_t kMediaFrameNonceBytes = 24;

/** The relay QoS class of a channel's frames (not a codec): audio in order, video latest-wins (MEDIA_CHANNELS.md). */
enum class MediaChannelType : uint8_t {
  ReliableOrdered = 0,
  LatestLossy = 1,
  BestEffort = 2,
};

/** The QoS class a sender stamps on a channel's frames: audio in order, video latest-wins. */
MediaChannelType MediaChannelTypeFor(uint16_t channel_id);

/** One frame: the clear header's fields, and `payload` — sealed (ciphertext + tag) on the wire, opaque to a hop. */
struct MediaDataFrame {
  uint32_t stream_id = 0;
  /** A media channel (MediaChannel.h): 0 audio, 0x11–0x1F video levels; one byte on the wire. */
  uint16_t channel_id = 0;
  MediaChannelType channel_type = MediaChannelType::BestEffort;
  uint32_t seq = 0;
  /** 1 on a keyframe (video), else 0. */
  uint8_t mark = 0;
  /** The sender session's random instance id (the nonce's): 0 only on frames that are not sealed. */
  uint64_t instance = 0;
  std::vector<uint8_t> payload;
};

std::array<uint8_t, kMediaFrameHeaderBytes> EncodeMediaFrameHeader(const MediaDataFrame& frame);
/** The header then the payload. */
std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame);
/** A frame from the wire; an older version, a bad type or a short body is refused. */
Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body);

/** A fresh sender instance: random (libsodium) and nonzero. Drawn wherever a sender's seq counters start. */
uint64_t NewMediaSenderInstance();

/** The derived nonce: channel, stream, instance, seq, epoch, then zeros. */
std::array<uint8_t, kMediaFrameNonceBytes> MediaFrameNonce(uint8_t channel, uint32_t stream_id, uint64_t instance,
                                                           uint32_t seq, uint32_t media_epoch);

/**
 * Seal `plaintext` into `frame.payload` under `media_key`, the frame's header and `context` as associated data. The
 * context is the feature's label + session — `call-media-sfu|<call_id>`, `call-media|<call_id>`,
 * `broadcast-media|<program_id>|<join_handle>` — so a frame never opens under another feature or session.
 */
Roe<void> SealMediaFrame(const ByteVector& media_key, const std::string& context, uint32_t media_epoch,
                         MediaDataFrame& frame, const std::vector<uint8_t>& plaintext);

/** The plaintext of a sealed frame, or an error when the tag fails (another key, epoch, context, or altered). */
Roe<std::vector<uint8_t>> OpenMediaFrame(const ByteVector& media_key, const std::string& context, uint32_t media_epoch,
                                         const MediaDataFrame& frame);

/**
 * A sender's guard against a repeated nonce: under one instance and key epoch, each channel's seq must move forward.
 * A frame it refuses is dropped, never sealed (a bug costs a frame, not the key). One per sending side; thread-safe
 * (a media engine's audio and video threads send through the same side).
 */
class MediaFrameSendGuard {
public:
  bool Admit(uint64_t instance, uint32_t media_epoch, uint8_t channel, uint32_t seq);

private:
  std::mutex mu_;
  std::map<std::tuple<uint64_t, uint32_t, uint8_t>, uint32_t> last_;
};

} // namespace pp::mesh
