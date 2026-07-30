#include "libp2p/integration/host/Reachability.h"

#include <gtest/gtest.h>

TEST(ReachabilityTest, PrivateIpv4Classification) {
  EXPECT_TRUE(pbr::IsPrivateIpv4("10.0.0.1"));
  EXPECT_TRUE(pbr::IsPrivateIpv4("192.168.1.4"));
  EXPECT_TRUE(pbr::IsPrivateIpv4("172.16.0.1"));
  EXPECT_FALSE(pbr::IsPublicIpv4("10.0.0.1"));
  EXPECT_TRUE(pbr::IsPublicIpv4("8.8.8.8"));
}

TEST(ReachabilityTest, ClassifyReachabilitySignals) {
  pbr::ReachabilitySignals blocked;
  blocked.seed_dial_ok = false;
  EXPECT_EQ(pbr::ClassifyReachability(blocked), pbr::ReachabilityStatus::Blocked);

  pbr::ReachabilitySignals reachable;
  reachable.seed_dial_ok = true;
  reachable.dial_back_ok = true;
  EXPECT_EQ(pbr::ClassifyReachability(reachable), pbr::ReachabilityStatus::Reachable);

  pbr::ReachabilitySignals outbound;
  outbound.seed_dial_ok = true;
  outbound.dial_back_ok = false;
  EXPECT_EQ(pbr::ClassifyReachability(outbound), pbr::ReachabilityStatus::OutboundOnly);
}

TEST(ReachabilityTest, BuildProbeTargetsIncludesPeerId) {
  const auto targets = pbr::BuildReachabilityProbeTargets("/ip4/0.0.0.0/tcp/18517", "12D3KooWTest", {}, "");
  ASSERT_FALSE(targets.empty());
  EXPECT_NE(targets.back().find("/p2p/12D3KooWTest"), std::string::npos);
}

TEST(ReachabilityTest, SkipUpnpForPublicListen) {
  EXPECT_TRUE(pbr::ShouldSkipUpnpForListen("/ip4/203.0.113.10/tcp/443"));
  EXPECT_FALSE(pbr::ShouldSkipUpnpForListen("/ip4/0.0.0.0/tcp/18517"));
}
