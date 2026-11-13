#pragma once

#include "common/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace pp::mesh {

using ByteVector = std::vector<uint8_t>;

/**
 * A media frame (A029 H2, frame v4): one header for every path a frame takes — through a relay hop (calls,
 * broadcast) or straight to the peer (a 1:1 call) — and one end-to-end seal. Big endian:
 *
 *   core (22 B, fixed offsets):
 *     [version (high nibble) | channel type << 2 | mark][channel u8][stream u32][seq u32][capture_ts u32][instance u64]
 *   extension block:
 *     [ext_len u8][extension …]           ext_len = the bytes of extensions that follow, 0 = none, at most 64
 *     extension = [type u8][len u8][value: len bytes]
 *   [payload sealed with the media key][tag 16]
 *
 * The header — core and extension block — travels in the clear (a hop routes by stream and channel, drops stale lossy
 * video by type, seq and mark) and is, with the owning feature's context, the seal's associated data: nothing can
 * add, drop or alter an extension or the timestamp undetected. The nonce is derived from the core, never sent:
 * channel, stream, instance, seq, key epoch. `instance` is random per sender session (the media engine picks it where
 * its counters start), so a sender that restarts under the same key, or two senders with one stream id, never repeat
 * a nonce. A sender keeps every (channel, seq) once per instance and epoch (MediaFrameSendGuard).
 *
 * `seq` orders frames and shows losses; `capture_ts` is when the frame was captured, so a receiver schedules playout
 * by time (a sender that pauses sends no frames and its seq stands still, while its clock moves on).
 *
 * Extensions carry every optional field after v4: a new one gets a type here, never a new version. The version
 * changes only when the core, the seal or the nonce changes meaning. The rules, for every reader:
 * - type 0 is invalid; the high bit of `type` (kMediaFrameExtensionMustUnderstand) says "must understand";
 * - an unknown type is skipped (kept in `extensions`, so the header — the seal's associated data — stays as it came);
 * - an unknown must-understand type drops the frame at the receiver that opens it (DecodeMediaDataFrame). A hop routes
 *   by the core alone and forwards every well-formed frame as it came (DecodeMediaDataFrameHeader);
 * - ext_len past 64, an extension that runs past the block or the frame, a type 0, or bytes left in the block that
 *   are no whole extension: the frame is malformed and dropped, by a hop too.
 * Registry (MediaFrameExtensionKnown): none defined yet. v4 does not interoperate with v3: a reader takes v4 only.
 */
inline constexpr uint8_t kMediaFrameVersion = 4;
/** The fixed part of the header, before the extension block. */
inline constexpr size_t kMediaFrameCoreBytes = 1 + 1 + 4 + 4 + 4 + 8;
/** The shortest header: the core and an empty extension block (ext_len 0). */
inline constexpr size_t kMediaFrameMinHeaderBytes = kMediaFrameCoreBytes + 1;
/** The most bytes of extensions a frame carries (ext_len). */
inline constexpr size_t kMediaFrameMaxExtensionBytes = 64;
inline constexpr size_t kMediaFrameMaxHeaderBytes = kMediaFrameMinHeaderBytes + kMediaFrameMaxExtensionBytes;
/** The extension type that is never valid. */
inline constexpr uint8_t kMediaFrameExtensionInvalid = 0;
/** The high bit of an extension's type: a receiver that does not know the type drops the frame. */
inline constexpr uint8_t kMediaFrameExtensionMustUnderstand = 0x80;
inline constexpr size_t kMediaFrameTagBytes = 16;
inline constexpr size_t kMediaFrameNonceBytes = 24;

/**
 * What a relay may do with a channel's frames (its QoS class, not a codec), two bits of the header. Every frame rides
 * best effort; this says only whether a relay may drop a stale one (MEDIA_CHANNELS.md § Channel type).
 */
enum class MediaChannelType : uint8_t {
  /** Forwarded as it came, every frame (audio). */
  Forward = 0,
  /** A non-keyframe older than one a subscriber already has is dropped (video). */
  LatestLossy = 1,
};

/** The class a sender stamps on a channel's frames: audio Forward, video LatestLossy. */
MediaChannelType MediaChannelTypeFor(uint16_t channel_id);

/** One extension of a frame's header: its type and value (the block holds at most 64 B, type and len bytes counted). */
struct MediaFrameExtension {
  uint8_t type = kMediaFrameExtensionInvalid;
  std::vector<uint8_t> value;
  bool operator==(const MediaFrameExtension&) const = default;
};

/** One frame: the clear header's fields, and `payload` — sealed (ciphertext + tag) on the wire, opaque to a hop. */
struct MediaDataFrame {
  uint32_t stream_id = 0;
  /** A media channel (MediaChannel.h): 0 audio, 0x11–0x1F video levels; one byte on the wire. */
  uint16_t channel_id = 0;
  MediaChannelType channel_type = MediaChannelType::Forward;
  uint32_t seq = 0;
  /** 1 on a keyframe (video), else 0. */
  uint8_t mark = 0;
  /**
   * When the frame's first sample was captured: ms since the sender session started (its own clock, one origin per
   * `instance`), wrapping at 2^32 ms. A receiver compares only one instance's timestamps, modulo 2^32.
   */
  uint32_t capture_ts = 0;
  /** The sender session's random instance id (the nonce's): 0 only on frames that are not sealed. */
  uint64_t instance = 0;
  /** The extension block, in wire order; empty by default. Every one decoded is kept, known or not. */
  std::vector<MediaFrameExtension> extensions;
  std::vector<uint8_t> payload;
};

/**
 * The extension registry: whether this build understands `type`. None is defined yet; a new optional field adds its
 * type here (and to the frame comment above), with the must-understand bit set only if a receiver that ignores it
 * would misread the frame.
 */
bool MediaFrameExtensionKnown(uint8_t type);
/** Extensions a sender may stamp: no type 0, at most 64 B in all (type and len bytes counted). */
Roe<void> CheckMediaFrameExtensions(const std::vector<MediaFrameExtension>& extensions);
/** The encoded header's length: the core, ext_len, and the extensions. */
size_t MediaFrameHeaderBytes(const MediaDataFrame& frame);
/** The header (core and extension block); `frame.extensions` must pass CheckMediaFrameExtensions. */
std::vector<uint8_t> EncodeMediaFrameHeader(const MediaDataFrame& frame);
/** The header then the payload; `frame.extensions` must pass CheckMediaFrameExtensions (SealMediaFrame checks). */
std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame);
/**
 * A frame from the wire, at the receiver: another version, a bad type, a short body, a malformed extension block or an
 * unknown must-understand extension is refused.
 */
Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body);
/**
 * The frame's header only (`payload` left empty), at a hop: it forwards the encoded bytes as they came and needs no
 * copy of the payload. A malformed header is refused; must-understand extensions are the receiver's (not checked).
 */
Roe<MediaDataFrame> DecodeMediaDataFrameHeader(std::span<const uint8_t> body);

/** A fresh sender instance: random (libsodium) and nonzero. Drawn wherever a sender's seq counters start. */
uint64_t NewMediaSenderInstance();

/** The derived nonce: channel, stream, instance, seq, epoch, then zeros. */
std::array<uint8_t, kMediaFrameNonceBytes> MediaFrameNonce(uint8_t channel, uint32_t stream_id, uint64_t instance,
                                                           uint32_t seq, uint32_t media_epoch);

/**
 * Seal `plaintext` into `frame.payload` under `media_key`, the frame's whole header (core and extension block) and
 * `context` as associated data — set `capture_ts` and `extensions` before sealing. The
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
