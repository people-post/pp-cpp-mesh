#include "domain/mesh/dht/DhtRateLimiter.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(DhtRateLimiterTest, AllowsUntilWindowCap) {
  DhtRateLimiter limiter(3, 60);
  EXPECT_TRUE(limiter.Allow("peer-a"));
  EXPECT_TRUE(limiter.Allow("peer-a"));
  EXPECT_TRUE(limiter.Allow("peer-a"));
  EXPECT_FALSE(limiter.Allow("peer-a"));
  EXPECT_TRUE(limiter.Allow("peer-b"));
}

TEST(DhtRateLimiterTest, EmptyPeerSharesBucket) {
  DhtRateLimiter limiter(1, 60);
  EXPECT_TRUE(limiter.Allow(""));
  EXPECT_FALSE(limiter.Allow(""));
}

} // namespace
} // namespace pbr
