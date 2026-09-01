#include "base/mesh/MediaRelayTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MediaFrameCodecTest, RoundTripHeaderAndPayload) {
  MediaDataFrame in;
  in.stream_id = 42;
  in.channel_id = 7;
  in.channel_type = MediaChannelType::LatestLossy;
  in.seq = 99;
  in.mark = 1;
  in.payload = {1, 2, 3, 4, 5};
  const auto bytes = EncodeMediaDataFrame(in);
  auto out = DecodeMediaDataFrame(bytes);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->stream_id, 42u);
  EXPECT_EQ(out->channel_id, 7u);
  EXPECT_EQ(out->channel_type, MediaChannelType::LatestLossy);
  EXPECT_EQ(out->seq, 99u);
  EXPECT_EQ(out->mark, 1);
  EXPECT_EQ(out->payload, in.payload);
}

} // namespace
} // namespace pbr
