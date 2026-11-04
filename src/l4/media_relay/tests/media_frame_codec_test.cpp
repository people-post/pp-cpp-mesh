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
  f.instance = 0x1112131415161718ull;
  return f;
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
  EXPECT_EQ(bytes.size(), kMediaFrameHeaderBytes + in.payload.size());
  auto out = DecodeMediaDataFrame(bytes);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->stream_id, in.stream_id);
  EXPECT_EQ(out->channel_id, in.channel_id);
  EXPECT_EQ(out->channel_type, MediaChannelType::LatestLossy);
  EXPECT_EQ(out->seq, in.seq);
  EXPECT_EQ(out->mark, 1);
  EXPECT_EQ(out->instance, in.instance);
  EXPECT_EQ(out->payload, in.payload);
}

// A029 H2: 18 bytes, big endian — what a hop and every app parse.
TEST(MediaFrameCodecTest, GoldenHeaderLayout) {
  const auto header = EncodeMediaFrameHeader(Sample());
  const std::array<uint8_t, kMediaFrameHeaderBytes> expected = {0x35, 0x11, 0x0A, 0x0B, 0x0C, 0x0D, 0x01, 0x02, 0x03,
                                                                0x04, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
  EXPECT_EQ(header, expected); // version 3, LatestLossy (1) << 2, mark 1
}

// The frames of a build before H2 (relay frame v1, end-to-end body v2) are refused, not misread.
TEST(MediaFrameCodecTest, OlderVersionsAreRefused) {
  auto bytes = EncodeMediaDataFrame(Sample());
  for (const uint8_t first : {uint8_t{0x01}, uint8_t{0x02}, uint8_t{0x45}}) {
    bytes[0] = first;
    EXPECT_FALSE(DecodeMediaDataFrame(bytes)) << int(first);
  }
  bytes[0] = 0x3C; // version 3, channel type 3 (none)
  EXPECT_FALSE(DecodeMediaDataFrame(bytes));
  EXPECT_FALSE(DecodeMediaDataFrame(std::vector<uint8_t>(kMediaFrameHeaderBytes - 1, 0x30)));
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
  f.channel_type = MediaChannelType::BestEffort;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.instance ^= 1;
  EXPECT_TRUE(fails(f));
  f = sealed;
  f.payload[0] ^= 1;
  EXPECT_TRUE(fails(f));
  EXPECT_TRUE(fails(sealed, ByteVector(32, 0x7e)));
  EXPECT_TRUE(fails(sealed, CountingKey(), "call-media|call-1")) << "another feature's context";
  EXPECT_TRUE(fails(sealed, CountingKey(), "call-media-sfu|call-2")) << "another call";
  EXPECT_TRUE(fails(sealed, CountingKey(), kContext, 4)) << "another epoch";
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
  // Cross-checked with libsodium from Python: the nonce and associated data built by hand.
  EXPECT_EQ(Hex(f.payload), "68bcbba0b054709c8a28b16b9b1b151fd0ec9827");
}

TEST(MediaFrameCodecTest, AudioIsOrderedAndVideoLatestWins) {
  EXPECT_EQ(MediaChannelTypeFor(kMediaChannelAudio), MediaChannelType::ReliableOrdered);
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
