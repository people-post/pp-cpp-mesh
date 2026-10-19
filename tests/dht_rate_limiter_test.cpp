#include "domain/mesh/dht/DhtRateLimiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

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

// Peers with no op left in the window are dropped (every peer id ever seen used to keep an entry).
TEST(DhtRateLimiterTest, IdlePeersAreDropped) {
  DhtRateLimiter limiter(1000, 1);
  for (int i = 0; i < 300; ++i) {
    EXPECT_TRUE(limiter.Allow("peer-" + std::to_string(i)));
  }
  EXPECT_GE(limiter.TrackedPeers(), 44u);  // the first sweep ran at 256 grants, all still in window
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // everyone leaves the window
  for (size_t i = 0; i < DhtRateLimiter::kSweepEveryGrants; ++i) {
    EXPECT_TRUE(limiter.Allow("active"));
  }
  EXPECT_EQ(limiter.TrackedPeers(), 1u);
}

} // namespace
} // namespace pbr
