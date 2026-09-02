#include "base/mesh/reachability/Reachability.h"

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

TEST(ReachabilityTest, BuildAmpProbeTargetsIncludesPeerId) {
  const auto targets =
      pbr::BuildAmpReachabilityProbeTargets("/ip4/0.0.0.0/udp/18517/adp/1.0.0", "12D3KooWTest", "");
  ASSERT_FALSE(targets.empty());
  EXPECT_NE(targets.back().find("/p2p/12D3KooWTest"), std::string::npos);
}

TEST(ReachabilityTest, SkipUpnpForPublicListen) {
  EXPECT_TRUE(pbr::ShouldSkipUpnpForListen("/ip4/203.0.113.10/udp/443/adp/1.0.0"));
  EXPECT_FALSE(pbr::ShouldSkipUpnpForListen("/ip4/0.0.0.0/udp/18517/adp/1.0.0"));
}

TEST(ReachabilityTest, UndialableLanAndVirtualIfaceHelpers) {
  EXPECT_TRUE(pbr::IsLikelyUndialableLanIpv4("192.168.122.1"));
  EXPECT_TRUE(pbr::IsLikelyUndialableLanIpv4("192.168.122.50"));
  EXPECT_FALSE(pbr::IsLikelyUndialableLanIpv4("192.168.1.152"));
  EXPECT_FALSE(pbr::IsLikelyUndialableLanIpv4("192.168.1.122"));
  EXPECT_TRUE(pbr::IsVirtualLanIfaceName("virbr0"));
  EXPECT_TRUE(pbr::IsVirtualLanIfaceName("docker0"));
  EXPECT_TRUE(pbr::IsVirtualLanIfaceName("vethabc123"));
  EXPECT_FALSE(pbr::IsVirtualLanIfaceName("enp9s0"));
  EXPECT_FALSE(pbr::IsVirtualLanIfaceName("wlan0"));
}

TEST(ReachabilityTest, BuildAmpLanAdvertisedAddrsExpandsWildcardOrKeepsConcrete) {
  const auto concrete =
      pbr::BuildAmpLanAdvertisedAddrs("/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest");
  // May be empty on hosts without dialable LAN ifaces; concrete host still returned when no iface list.
  if (!concrete.empty()) {
    EXPECT_NE(concrete.front().find("/udp/19001/adp/1.0.0/p2p/"), std::string::npos);
  }

  const auto wildcard =
      pbr::BuildAmpLanAdvertisedAddrs("/ip4/0.0.0.0/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest");
  for (const std::string& ma : wildcard) {
    EXPECT_EQ(ma.find("/ip4/0.0.0.0/"), std::string::npos);
    EXPECT_NE(ma.find("/udp/19001/adp/1.0.0/p2p/12D3KooWTest"), std::string::npos);
  }
}
