#include "domain/mesh/reach/PeerMediaRelayCaps.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// V030: unknown peers are not relay-capable; only a new capable ad reports "learned".
TEST(PeerMediaRelayCapsTest, LearnsCapableAdsAndFailsClosed) {
  PeerMediaRelayCaps caps;
  EXPECT_FALSE(caps.Has("12D3a"));
  EXPECT_FALSE(caps.Note("", true));
  EXPECT_TRUE(caps.Note("12D3a", true));
  EXPECT_FALSE(caps.Note("12D3a", true)) << "already known capable";
  EXPECT_FALSE(caps.Note("12D3b", false));
  EXPECT_TRUE(caps.Has("12D3a"));
  EXPECT_FALSE(caps.Has("12D3b"));
  EXPECT_EQ(caps.ListCapable(), std::vector<std::string>{"12D3a"});

  EXPECT_FALSE(caps.Note("12D3a", false)) << "withdrawn";
  EXPECT_FALSE(caps.Has("12D3a"));
  EXPECT_TRUE(caps.Note("12D3a", true)) << "capable again counts as learned";
}

} // namespace
} // namespace pbr
