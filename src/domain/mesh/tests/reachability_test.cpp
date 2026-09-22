#include "domain/mesh/reachability/Reachability.h"

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

TEST(ReachabilityTest, RankAmpDialMultiaddrsPrefersGlobalIpv6OverPrivateIpv4) {
  const std::string v6 = "/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string pub4 = "/ip4/203.0.113.10/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string lan4 = "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  EXPECT_LT(pbr::AmpDialMultiaddrRank(v6), pbr::AmpDialMultiaddrRank(pub4));
  EXPECT_LT(pbr::AmpDialMultiaddrRank(pub4), pbr::AmpDialMultiaddrRank(lan4));

  const auto ranked = pbr::RankAmpDialMultiaddrs({lan4, pub4, v6});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0], v6);
  EXPECT_EQ(ranked[1], pub4);
  EXPECT_EQ(ranked[2], lan4);
}

TEST(ReachabilityTest, RankAmpDialMultiaddrsPrefersSameSubnetLanOverGlobalIpv6) {
  const std::string v6 = "/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string pub4 = "/ip4/203.0.113.10/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string same_lan = "/ip4/192.168.0.109/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string other_lan = "/ip4/10.68.189.3/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  pbr::AmpDialLocalContext ctx;
  ctx.lan_ipv4_hosts = {"192.168.0.105"};
  ctx.has_global_ipv6 = true;

  const auto ranked = pbr::RankAmpDialMultiaddrs({other_lan, v6, pub4, same_lan}, ctx);
  ASSERT_EQ(ranked.size(), 4u);
  EXPECT_EQ(ranked[0], same_lan);  // peer on our /24 beats everything else
  EXPECT_EQ(ranked[1], v6);
  EXPECT_EQ(ranked[2], pub4);
  EXPECT_EQ(ranked[3], other_lan);
}

TEST(ReachabilityTest, RankAmpDialMultiaddrsDemotesIpv6WhenLocalHasNoGlobalIpv6) {
  const std::string v6 = "/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string pub4 = "/ip4/203.0.113.10/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string lan4 = "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  pbr::AmpDialLocalContext ctx;
  ctx.has_global_ipv6 = false;  // v4-only socket cannot send to /ip6 at all

  const auto ranked = pbr::RankAmpDialMultiaddrs({lan4, v6, pub4}, ctx);
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0], pub4);
  EXPECT_EQ(ranked[1], lan4);
  EXPECT_EQ(ranked[2], v6);
}

TEST(ReachabilityTest, LinkLocalIpv4IsUndialable) {
  EXPECT_TRUE(pbr::IsLikelyUndialableLanIpv4("169.254.75.55"));
  EXPECT_TRUE(pbr::IsLikelyUndialableLanIpv4("169.254.116.242"));
  EXPECT_FALSE(pbr::IsLikelyUndialableLanIpv4("192.168.0.105"));
  EXPECT_FALSE(pbr::IsLikelyUndialableLanIpv4("10.68.189.3"));
}

TEST(ReachabilityTest, BuildAmpGlobalIpv6AdvertisedAddrsFromHosts) {
  const auto addrs = pbr::BuildAmpGlobalIpv6AdvertisedAddrs(
      "/ip4/0.0.0.0/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest",
      {"2001:db8::10", "fe80::1", "fd12::1", "::1"});
  ASSERT_EQ(addrs.size(), 1u);
  EXPECT_NE(addrs.front().find("/ip6/"), std::string::npos);
  EXPECT_NE(addrs.front().find("2001:db8::10"), std::string::npos);
  EXPECT_NE(addrs.front().find("/udp/19001/adp/1.0.0/p2p/12D3KooWTest"), std::string::npos);
}

TEST(ReachabilityTest, ReachableViaIpv6ContractUsesDialBackPlusGlobalIpv6) {
  // Product chrome: Reachable + has_global_ipv6 + dial_back_ok => "Reachable via IPv6".
  pbr::ReachabilitySignals signals;
  signals.seed_dial_ok = true;
  signals.dial_back_ok = true;
  signals.has_global_ipv6 = true;
  EXPECT_EQ(pbr::ClassifyReachability(signals), pbr::ReachabilityStatus::Reachable);
  EXPECT_TRUE(signals.has_global_ipv6);
  EXPECT_TRUE(signals.dial_back_ok);

  pbr::ReachabilitySignals outbound_only_v6;
  outbound_only_v6.seed_dial_ok = true;
  outbound_only_v6.dial_back_ok = false;
  outbound_only_v6.has_global_ipv6 = true;
  EXPECT_EQ(pbr::ClassifyReachability(outbound_only_v6), pbr::ReachabilityStatus::OutboundOnly);
}

TEST(ReachabilityTest, IsGlobalIpv6FiltersLinkLocalAndUla) {
  EXPECT_TRUE(pbr::IsGlobalIpv6("2001:db8::1"));
  EXPECT_FALSE(pbr::IsGlobalIpv6("fe80::1"));
  EXPECT_FALSE(pbr::IsGlobalIpv6("fd12::1"));
  EXPECT_FALSE(pbr::IsGlobalIpv6("::1"));
  EXPECT_FALSE(pbr::IsGlobalIpv6("192.168.1.1"));
}

TEST(ReachabilityTest, SkipUpnpForGlobalIpv6Listen) {
  EXPECT_TRUE(pbr::ShouldSkipUpnpForListen("/ip6/2001:db8::10/udp/18517/adp/1.0.0"));
  EXPECT_FALSE(pbr::ShouldSkipUpnpForListen("/ip6/::/udp/18517/adp/1.0.0"));
  EXPECT_FALSE(pbr::ShouldSkipUpnpForListen("/ip6/fe80::1/udp/18517/adp/1.0.0"));
}
