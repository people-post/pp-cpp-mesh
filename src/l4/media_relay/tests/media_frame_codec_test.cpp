#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "mesh/l4/shared/MediaFrame.h"
#include "mesh/types/MediaChannel.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace pp::mesh {
namespace {

MediaDataFrame Sample() {
  MediaDataFrame f;
  f.stream_id = 0x0A0B0C0Du;
  f.channel_id = 0x11;
  f.channel_type = MediaChannelType::LatestLossy;
  f.seq = 0x01020304u;
  f.mark = 1;
  f.capture_ts = 0x31323334u;
  f.instance = 0x1112131415161718ull;
  return f;
}

/** Two extensions of types no build knows: optional (0x05) and optional (0x7F, empty). */
std::vector<MediaFrameExtension> TwoUnknownOptional() {
  return {MediaFrameExtension{0x05, {0xAA, 0xBB}}, MediaFrameExtension{0x7F, {}}};
}

/** A frame's bytes with its extension block replaced by `block` as given (ext_len and all), then `tail`. */
std::vector<uint8_t> WithRawBlock(const std::vector<uint8_t>& block, const std::vector<uint8_t>& tail = {1, 2, 3}) {
  auto bytes = EncodeMediaFrameHeader(Sample());
  bytes.resize(kMediaFrameCoreBytes);
  bytes.insert(bytes.end(), block.begin(), block.end());
  bytes.insert(bytes.end(), tail.begin(), tail.end());
  return bytes;
}

ByteVector CountingKey() {
  ByteVector key(32);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }
  return key;
}

const std::vector<uint8_t> kOpus = {'o', 'p', 'u', 's'};
const std::string kContext = "call-media-sfu|call-1";

std::string Hex(const std::vector<uint8_t>& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

TEST(MediaFrameCodecTest, RoundTripHeaderAndPayload) {
  MediaDataFrame in = Sample();
  in.payload = {1, 2, 3, 4, 5};
  const auto bytes = EncodeMediaDataFrame(in);
  EXPECT_EQ(bytes.size(), kMediaFrameMinHeaderBytes + in.payload.size());
  auto out = DecodeMediaDataFrame(bytes);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->stream_id, in.stream_id);
  EXPECT_EQ(out->channel_id, in.channel_id);
  EXPECT_EQ(out->channel_type, MediaChannelType::LatestLossy);
  EXPECT_EQ(out->seq, in.seq);
  EXPECT_EQ(out->mark, 1);
  EXPECT_EQ(out->capture_ts, in.capture_ts);
  EXPECT_EQ(out->instance, in.instance);
  EXPECT_TRUE(out->extensions.empty());
  EXPECT_EQ(out->payload, in.payload);
}

// One extension, several, and a full 64-byte block: every one comes back in order, and the payload after them.
TEST(MediaFrameCodecTest, RoundTripWithExtensions) {
  const std::vector<std::vector<MediaFrameExtension>> cases = {
      {MediaFrameExtension{0x01, {0x42}}},
      {MediaFrameExtension{0x01, {}}, MediaFrameExtension{0x02, {1, 2, 3}}, MediaFrameExtension{0x7F, {9, 9}}},
      {MediaFrameExtension{0x03, std::vector<uint8_t>(30, 0x11)},
       MediaFrameExtension{0x04, std::vector<uint8_t>(30, 0x22)}}, // 2 + 30 + 2 + 30 = 64
  };
  for (const auto& extensions : cases) {
    MediaDataFrame in = Sample();
    in.extensions = extensions;
    in.payload = {7, 8, 9};
    ASSERT_TRUE(CheckMediaFrameExtensions(in.extensions));
    const auto bytes = EncodeMediaDataFrame(in);
    EXPECT_EQ(bytes.size(), MediaFrameHeaderBytes(in) + in.payload.size());
    EXPECT_EQ(bytes[kMediaFrameCoreBytes], MediaFrameHeaderBytes(in) - kMediaFrameMinHeaderBytes) << "ext_len";
    auto out = DecodeMediaDataFrame(bytes);
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out->capture_ts, in.capture_ts);
    EXPECT_EQ(out->extensions, in.extensions);
    EXPECT_EQ(out->payload, in.payload);
    EXPECT_EQ(EncodeMediaDataFrame(*out), bytes) << "decode then encode gives the bytes as they came";
    auto header = DecodeMediaDataFrameHeader(bytes);
    ASSERT_TRUE(header);
    EXPECT_EQ(header->extensions, in.extensions);
    EXPECT_TRUE(header->payload.empty());
  }
}

// Frame v4, big endian — what a hop and every app parse: the 22-byte core, then ext_len and the extensions.
TEST(MediaFrameCodecTest, GoldenHeaderLayout) {
  const std::vector<uint8_t> expected = {0x45, 0x11, 0x0A, 0x0B, 0x0C, 0x0D, 0x01, 0x02, 0x03, 0x04, 0x31, 0x32,
                                         0x33, 0x34, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x00};
  EXPECT_EQ(EncodeMediaFrameHeader(Sample()), expected); // version 4, LatestLossy (1) << 2, mark 1; no extensions
  MediaDataFrame f = Sample();
  f.extensions = TwoUnknownOptional();
  std::vector<uint8_t> with = expected;
  with.back() = 6; // ext_len
  for (const uint8_t b : {0x05, 0x02, 0xAA, 0xBB, 0x7F, 0x00}) {
    with.push_back(b);
  }
  EXPECT_EQ(EncodeMediaFrameHeader(f), with);
}

// v4 is a clean cut: the frames of an older build (v3 and the relay frame v1 / end-to-end body v2 before it) are
// refused, not misread.
TEST(MediaFrameCodecTest, OlderVersionsAreRefused) {
  // A v3 frame: [0x35][channel][stream][seq][instance] then a sealed payload — 18 B of header, no timestamp.
  const std::vector<uint8_t> v3 = {0x35, 0x11, 0x0A, 0x0B, 0x0C, 0x0D, 0x01, 0x02, 0x03, 0x04, 0x11, 0x12,
                                   0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 'o',  'p',  'u',  's',  0x00, 0x00};
  EXPECT_FALSE(DecodeMediaDataFrame(v3));
  EXPECT_FALSE(DecodeMediaDataFrameHeader(v3)) << "a hop refuses it too";
  auto bytes = EncodeMediaDataFrame(Sample());
  for (const uint8_t first : {uint8_t{0x01}, uint8_t{0x02}, uint8_t{0x35}, uint8_t{0x55}}) {
    bytes[0] = first;
    EXPECT_FALSE(DecodeMediaDataFrame(bytes)) << int(first);
  }
  for (const uint8_t first : {uint8_t{0x48}, uint8_t{0x4C}}) { // version 4, channel type 2 (best effort, gone) and 3
    bytes[0] = first;
    EXPECT_FALSE(DecodeMediaDataFrame(bytes)) << "channel type " << ((first >> 2) & 3);
  }
  EXPECT_FALSE(DecodeMediaDataFrame(std::vector<uint8_t>(kMediaFrameMinHeaderBytes - 1, 0x40)));
}

// An extension this build does not know, without the must-understand bit, is skipped: the frame decodes, its payload
// intact, and the extension is kept (the header is the seal's associated data).
TEST(MediaFrameCodecTest, AnUnknownOptionalExtensionIsSkipped) {
  MediaDataFrame in = Sample();
  in.extensions = TwoUnknownOptional();
  in.payload = {4, 5, 6};
  ASSERT_FALSE(MediaFrameExtensionKnown(0x05));
  auto out = DecodeMediaDataFrame(EncodeMediaDataFrame(in));
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->payload, in.payload);
  EXPECT_EQ(out->seq, in.seq);
  EXPECT_EQ(out->extensions, in.extensions);
}

// An unknown extension with the must-understand bit drops the frame at the receiver. A hop routes by the core and
// forwards it: the receiver decides.
TEST(MediaFrameCodecTest, AnUnknownMustUnderstandExtensionDropsTheFrame) {
  MediaDataFrame in = Sample();
  in.extensions = {MediaFrameExtension{0x05, {1}}, MediaFrameExtension{0x85, {2}}};
  in.payload = {4, 5, 6};
  ASSERT_FALSE(MediaFrameExtensionKnown(0x85));
  const auto bytes = EncodeMediaDataFrame(in);
  EXPECT_FALSE(DecodeMediaDataFrame(bytes));
  auto at_hop = DecodeMediaDataFrameHeader(bytes);
  ASSERT_TRUE(at_hop) << at_hop.error().message;
  EXPECT_EQ(at_hop->stream_id, in.stream_id);
  EXPECT_EQ(at_hop->extensions, in.extensions);
  in.extensions = {MediaFrameExtension{0x80, {}}};
  EXPECT_FALSE(DecodeMediaDataFrame(EncodeMediaDataFrame(in)));
}

// A malformed extension block drops the frame, at a hop too: past 64 bytes, an extension past its block or the
// frame, a type 0, or bytes left that are no whole extension.
TEST(MediaFrameCodecTest, AMalformedExtensionBlockDropsTheFrame) {
  ASSERT_TRUE(DecodeMediaDataFrame(WithRawBlock({0x00}))) << "an empty block is fine";
  ASSERT_TRUE(DecodeMediaDataFrame(WithRawBlock({0x02, 0x05, 0x00}))) << "one empty extension is fine";
  std::vector<uint8_t> past_64 = {65, 0x05, 63};
  past_64.resize(1 + 65, 0x11); // one extension of 65 bytes, all there
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> malformed = {
      {"ext_len past 64", WithRawBlock(past_64)},
      {"an extension past its block", WithRawBlock({0x03, 0x05, 0x05, 0x01}, {1, 2, 3, 4, 5, 6, 7, 8})},
      {"a block past the frame", WithRawBlock({0x0A, 0x05, 0x08, 0x01}, {})},
      {"a zero-length type 0", WithRawBlock({0x02, 0x00, 0x00})},
      {"a type 0 after a good one", WithRawBlock({0x05, 0x05, 0x01, 0xAA, 0x00, 0x00})},
      {"a trailing byte in the block", WithRawBlock({0x03, 0x05, 0x00, 0x07})},
  };
  for (const auto& [why, bytes] : malformed) {
    EXPECT_FALSE(DecodeMediaDataFrame(bytes)) << why;
    EXPECT_FALSE(DecodeMediaDataFrameHeader(bytes)) << why << " (at a hop)";
  }
}

// A sender never stamps what a reader would refuse: no type 0, no block past 64 B.
TEST(MediaFrameCodecTest, BadExtensionsAreNotSealed) {
  const std::vector<std::vector<MediaFrameExtension>> bad = {
      {MediaFrameExtension{0x00, {}}},
      {MediaFrameExtension{0x01, std::vector<uint8_t>(256, 0)}},
      {MediaFrameExtension{0x01, std::vector<uint8_t>(31, 0)}, MediaFrameExtension{0x02, std::vector<uint8_t>(32, 0)}},
  };
  for (const auto& extensions : bad) {
    MediaDataFrame f = Sample();
    f.extensions = extensions;
    EXPECT_FALSE(CheckMediaFrameExtensions(f.extensions));
    EXPECT_FALSE(SealMediaFrame(CountingKey(), kContext, 1, f, kOpus));
  }
}

TEST(MediaFrameCodecTest, NonceIsChannelStreamInstanceSeqEpochThenZeros) {
  const auto nonce = MediaFrameNonce(0x11, 0x0A0B0C0Du, 0x1112131415161718ull, 0x01020304u, 0x21222324u);
  const std::array<uint8_t, kMediaFrameNonceBytes> expected = {0x11, 0x0A, 0x0B, 0x0C, 0x0D, 0x11, 0x12, 0x13,
                                                               0x14, 0x15, 0x16, 0x17, 0x18, 0x01, 0x02, 0x03,
                                                               0x04, 0x21, 0x22, 0x23, 0x24, 0x00, 0x00, 0x00};
  EXPECT_EQ(nonce, expected);
}

// Every input that tells two frames apart tells their nonces apart: audio and the video levels (simulcast shares a
// seq across levels), two senders with one stream id (devices of one account), an engine restart under one key, and
// epochs.
TEST(MediaFrameCodecTest, NoncesNeverRepeatAcrossChannelsStreamsInstancesAndEpochs) {
  std::set<std::array<uint8_t, kMediaFrameNonceBytes>> seen;
  size_t made = 0;
  for (const uint8_t channel : {uint8_t{0x00}, uint8_t{0x11}, uint8_t{0x12}}) {
    for (const uint32_t stream : {7u, 8u}) {
      for (const uint64_t instance : {1ull, 2ull, 0xffffffffffffffffull}) {
        for (const uint32_t epoch : {1u, 2u}) {
          for (uint32_t seq = 1; seq <= 200; ++seq) {
            seen.insert(MediaFrameNonce(channel, stream, instance, seq, epoch));
            ++made;
          }
        }
      }
    }
  }
  EXPECT_EQ(seen.size(), made);
}

TEST(MediaFrameCodecTest, ASealedFrameOpensAndCarriesItsTag) {
  MediaDataFrame frame = Sample();
  ASSERT_TRUE(SealMediaFrame(CountingKey(), kContext, 3, frame, kOpus));
  EXPECT_EQ(frame.payload.size(), kOpus.size() + kMediaFrameTagBytes);
  auto wire = DecodeMediaDataFrame(EncodeMediaDataFrame(frame));
  ASSERT_TRUE(wire);
  auto opened = OpenMediaFrame(CountingKey(), kContext, 3, *wire);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(*opened, kOpus);
}

// The header is associated data: a hop that changes where a frame goes or what it says about it breaks the tag.
TEST(MediaFrameCodecTest, AnAlteredHeaderOrAnotherKeyContextOrEpochDoesNotOpen) {
  MediaDataFrame sealed = Sample();
  ASSERT_TRUE(SealMediaFrame(CountingKey(), kContext, 3, sealed, kOpus));
  const auto fails = [&](MediaDataFrame f, const ByteVector& key = CountingKey(), const std::string& context = kContext,
                         uint32_t epoch = 3) { return !OpenMediaFrame(key, context, epoch, f); };
  auto f = sealed;
  f.stream_id ^= 1;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.channel_id = 0x12;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.seq += 1;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.mark = 0;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.channel_type = MediaChannelType::Forward;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.instance ^= 1;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.capture_ts += 1;
  EXPECT_TRUE(fails(f)) << "the timestamp";
  f = sealed;
  f.extensions = {MediaFrameExtension{0x05, {}}};
  EXPECT_TRUE(fails(f)) << "an added extension";
  f = sealed;
  f.payload[0] ^= 1;
  EXPECT_TRUE(fails(f));
  EXPECT_TRUE(fails(sealed, ByteVector(32, 0x7e)));
  EXPECT_TRUE(fails(sealed, CountingKey(), "call-media|call-1")) << "another feature's context";
  EXPECT_TRUE(fails(sealed, CountingKey(), "call-media-sfu|call-2")) << "another call";
  EXPECT_TRUE(fails(sealed, CountingKey(), kContext, 4)) << "another epoch";
}

// The whole header is associated data: on the wire, a flipped byte of the timestamp or of the extension block — its
// length, a type, a len or a value — fails the open, and so does dropping or adding an extension.
TEST(MediaFrameCodecTest, TheTimestampAndTheExtensionBlockAreSealed) {
  MediaDataFrame sealed = Sample();
  sealed.extensions = TwoUnknownOptional();
  ASSERT_TRUE(SealMediaFrame(CountingKey(), kContext, 3, sealed, kOpus));
  const auto wire = EncodeMediaDataFrame(sealed);
  auto intact = DecodeMediaDataFrame(wire);
  ASSERT_TRUE(intact);
  ASSERT_TRUE(OpenMediaFrame(CountingKey(), kContext, 3, *intact));
  size_t opened_altered = 0;
  for (size_t at = 10; at < MediaFrameHeaderBytes(sealed); ++at) {
    if (at >= 14 && at < kMediaFrameCoreBytes) {
      continue; // the instance (covered above)
    }
    auto bytes = wire;
    bytes[at] ^= 0x01;
    auto f = DecodeMediaDataFrame(bytes);
    if (!f) {
      continue; // malformed now (ext_len, a len): dropped before the open
    }
    EXPECT_FALSE(OpenMediaFrame(CountingKey(), kContext, 3, *f)) << "byte " << at;
    ++opened_altered;
  }
  EXPECT_GE(opened_altered, 8u) << "the timestamp's 4 bytes, both types and both value bytes reach the open";
  auto f = *intact;
  f.extensions.pop_back();
  EXPECT_FALSE(OpenMediaFrame(CountingKey(), kContext, 3, f)) << "a dropped extension";
  f = *intact;
  f.extensions.push_back(MediaFrameExtension{0x06, {1}});
  EXPECT_FALSE(OpenMediaFrame(CountingKey(), kContext, 3, f)) << "an added extension";
  f = *intact;
  std::swap(f.extensions[0], f.extensions[1]);
  EXPECT_FALSE(OpenMediaFrame(CountingKey(), kContext, 3, f)) << "reordered extensions";
}

TEST(MediaFrameCodecTest, AFrameWithoutAnInstanceOrPastTheChannelByteIsNotSealed) {
  MediaDataFrame f = Sample();
  f.instance = 0;
  EXPECT_FALSE(SealMediaFrame(CountingKey(), kContext, 1, f, kOpus));
  f = Sample();
  f.channel_id = 0x100;
  EXPECT_FALSE(SealMediaFrame(CountingKey(), kContext, 1, f, kOpus));
}

// Fixed known answer: a change to the header, the nonce or the associated data fails here, not between two releases.
TEST(MediaFrameCodecTest, KnownAnswer) {
  MediaDataFrame f = Sample();
  ASSERT_TRUE(SealMediaFrame(CountingKey(), kContext, 3, f, kOpus));
  // Cross-checked with libsodium from Python (PyNaCl): the nonce and associated data built by hand.
  EXPECT_EQ(Hex(f.payload), "68bcbba0322c8072b70a65919d7e9c6af0beb19e");
  f = Sample();
  f.extensions = TwoUnknownOptional();
  ASSERT_TRUE(SealMediaFrame(CountingKey(), kContext, 3, f, kOpus));
  EXPECT_EQ(Hex(f.payload), "68bcbba0c8e2c5f18fce61be636ccffc4d2ed918");
}

TEST(MediaFrameCodecTest, AudioIsForwardedAndVideoLatestWins) {
  EXPECT_EQ(MediaChannelTypeFor(kMediaChannelAudio), MediaChannelType::Forward);
  EXPECT_EQ(MediaChannelTypeFor(VideoChannel(1)), MediaChannelType::LatestLossy);
}

TEST(MediaFrameCodecTest, SenderInstancesAreNonzeroAndDistinct) {
  std::set<uint64_t> seen;
  for (int i = 0; i < 64; ++i) {
    const uint64_t v = NewMediaSenderInstance();
    EXPECT_NE(v, 0u);
    seen.insert(v);
  }
  EXPECT_EQ(seen.size(), 64u);
}

TEST(MediaFrameSendGuardTest, EachChannelsSeqMustMoveForwardUnderOneInstanceAndEpoch) {
  MediaFrameSendGuard guard;
  EXPECT_TRUE(guard.Admit(1, 1, 0x00, 1));
  EXPECT_TRUE(guard.Admit(1, 1, 0x00, 2));
  EXPECT_FALSE(guard.Admit(1, 1, 0x00, 2)) << "a repeated seq would repeat the nonce";
  EXPECT_FALSE(guard.Admit(1, 1, 0x00, 1));
  EXPECT_TRUE(guard.Admit(1, 1, 0x11, 2)) << "another channel counts on its own (simulcast levels share a seq)";
  EXPECT_TRUE(guard.Admit(1, 1, 0x12, 2));
  EXPECT_TRUE(guard.Admit(2, 1, 0x00, 1)) << "a restarted engine is a new instance";
  EXPECT_TRUE(guard.Admit(1, 2, 0x00, 1)) << "a new epoch is a new key";
}

} // namespace
} // namespace pp::mesh
