#include "domain/mesh/l4/media_relay/MediaRelayVideoLevels.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

using Levels = std::vector<uint8_t>;

Levels Carry(const Levels& offer, int parallel, const MediaRelayVideoPolicy& policy) {
  auto carried = ChooseCarriedVideoLevels({offer, parallel}, policy);
  EXPECT_TRUE(carried) << carried.error().message;
  return carried ? *carried : Levels{};
}

MediaRelayVideoPolicy Serves(Levels levels, int carry = 1, bool strict = false) {
  return MediaRelayVideoPolicy{std::move(levels), carry, strict};
}

// B009: a desktop offers two levels (one at a time), a phone one; relays serve high, low or both.
TEST(MediaRelayVideoLevelsTest, EachRelayCarriesWhatItServesOrTheClosestOffered) {
  const Levels desktop{1, 2};
  const Levels phone{1};
  EXPECT_EQ(Carry(desktop, 1, Serves({2})), Levels{2});
  EXPECT_EQ(Carry(phone, 1, Serves({2})), Levels{1}) << "a high relay still carries a phone's low level";
  EXPECT_EQ(Carry(desktop, 1, Serves({1})), Levels{1});
  EXPECT_EQ(Carry(phone, 1, Serves({1})), Levels{1});
}

TEST(MediaRelayVideoLevelsTest, BothSidesLimitHowManyLevelsAreCarried) {
  const MediaRelayVideoPolicy both = Serves({1, 2}, /*carry=*/2);
  EXPECT_EQ(Carry({1, 2}, 2, both), (Levels{1, 2}));
  EXPECT_EQ(Carry({1, 2}, 1, both), Levels{2}) << "one encoder: the highest";
  EXPECT_EQ(Carry({1, 2}, 2, Serves({1, 2}, /*carry=*/1)), Levels{2}) << "the relay carries one";
  EXPECT_EQ(Carry({1, 2}, 2, MediaRelayVideoPolicy{}), (Levels{1, 2})) << "no policy: whatever is offered";
}

TEST(MediaRelayVideoLevelsTest, AStrictRelayRefusesInsteadOfFallingBack) {
  EXPECT_FALSE(ChooseCarriedVideoLevels({{1}, 1}, Serves({2}, 1, /*strict=*/true)));
  auto no_video = ChooseCarriedVideoLevels({{}, 1}, Serves({2}, 1, /*strict=*/true));
  ASSERT_TRUE(no_video) << "an offer without video is never refused (viewers, audio-only)";
  EXPECT_TRUE(no_video->empty());
}

TEST(MediaRelayVideoLevelsTest, FallbackPicksTheClosestAndTheHigherOnATie) {
  EXPECT_EQ(Carry({1, 5}, 1, Serves({4})), Levels{5});
  EXPECT_EQ(Carry({1, 3}, 1, Serves({2})), Levels{3});
}

TEST(MediaRelayVideoLevelsTest, InvalidAndDuplicateLevelsAreIgnored) {
  EXPECT_EQ(Carry({0, 2, 2, 16}, 2, MediaRelayVideoPolicy{}), Levels{2});
  EXPECT_TRUE(Carry({0, 16}, 1, Serves({1})).empty());
}

TEST(MediaRelayVideoLevelsTest, PublishLevelIsTheHighestOfferedAndCarried) {
  EXPECT_EQ(PublishVideoLevel({1, 2}, {1, 2}), 2);
  EXPECT_EQ(PublishVideoLevel({1, 2}, {2, 1}), 2) << "the answer's order is not trusted";
  EXPECT_EQ(PublishVideoLevel({1, 2}, {15, 1}), 1) << "a level never offered is not sent";
  EXPECT_EQ(PublishVideoLevel({1, 2}, {}), 0);
  EXPECT_EQ(PublishVideoLevel({}, {1}), 0);
}

} // namespace
} // namespace pbr
