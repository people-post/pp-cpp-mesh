#include "common/media/MediaChannel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// docs/contracts/MEDIA_CHANNELS.md: one byte, kind in the high nibble, level in the low one.
TEST(MediaChannelTest, KindAndLevelShareOneByte) {
  EXPECT_EQ(kMediaChannelAudio, 0x00);
  EXPECT_EQ(VideoChannel(1), 0x11);
  EXPECT_EQ(VideoChannel(2), 0x12);
  EXPECT_EQ(VideoChannel(kMaxVideoLevel), 0x1F);
  EXPECT_TRUE(IsAudioChannel(kMediaChannelAudio));
  EXPECT_FALSE(IsVideoChannel(kMediaChannelAudio));
  EXPECT_TRUE(IsVideoChannel(VideoChannel(2)));
  EXPECT_EQ(VideoLevelOf(VideoChannel(2)), 2);
  EXPECT_EQ(VideoLevelOf(kMediaChannelAudio), 0);
}

TEST(MediaChannelTest, OnlyLevelsOneToFifteenAreVideo) {
  EXPECT_FALSE(IsVideoChannel(0x10)) << "level 0 is not a video level";
  EXPECT_FALSE(IsVideoChannel(0x21)) << "unknown kind";
  EXPECT_FALSE(IsVideoChannel(0x0111)) << "the relay's u16 high byte is 0";
  EXPECT_FALSE(IsAudioChannel(0x01)) << "audio has no level";
  EXPECT_FALSE(IsVideoLevel(0));
  EXPECT_FALSE(IsVideoLevel(16));
}

} // namespace
} // namespace pbr
