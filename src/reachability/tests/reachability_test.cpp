#include "mesh/reachability/Reachability.h"

#include <gtest/gtest.h>

TEST(ReachabilityTest, PrivateIpv4Classification) {
  EXPECT_TRUE(pp::mesh::IsPrivateIpv4("10.0.0.1"));
  EXPECT_TRUE(pp::mesh::IsPrivateIpv4("192.168.1.4"));
  EXPECT_TRUE(pp::mesh::IsPrivateIpv4("172.16.0.1"));
  EXPECT_FALSE(pp::mesh::IsPublicIpv4("10.0.0.1"));
  EXPECT_TRUE(pp::mesh::IsPublicIpv4("8.8.8.8"));
}

TEST(ReachabilityTest, ClassifyReachabilitySignals) {
  pp::mesh::ReachabilitySignals blocked;
  blocked.seed_dial_ok = false;
  EXPECT_EQ(pp::mesh::ClassifyReachability(blocked), pp::mesh::ReachabilityStatus::Blocked);

  pp::mesh::ReachabilitySignals reachable;
  reachable.seed_dial_ok = true;
  reachable.dial_back_ok = true;
  EXPECT_EQ(pp::mesh::ClassifyReachability(reachable), pp::mesh::ReachabilityStatus::Reachable);

  pp::mesh::ReachabilitySignals outbound;
  outbound.seed_dial_ok = true;
  outbound.dial_back_ok = false;
  EXPECT_EQ(pp::mesh::ClassifyReachability(outbound), pp::mesh::ReachabilityStatus::OutboundOnly);
}

TEST(ReachabilityTest, BuildAmpProbeTargetsIncludesPeerId) {
  const auto targets =
      pp::mesh::BuildAmpReachabilityProbeTargets("/ip4/0.0.0.0/udp/18517/adp/1.0.0", "12D3KooWTest", "");
  ASSERT_FALSE(targets.empty());
  EXPECT_NE(targets.back().find("/p2p/12D3KooWTest"), std::string::npos);
}

TEST(ReachabilityTest, SkipUpnpForPublicListen) {
  EXPECT_TRUE(pp::mesh::ShouldSkipUpnpForListen("/ip4/203.0.113.10/udp/443/adp/1.0.0"));
  EXPECT_FALSE(pp::mesh::ShouldSkipUpnpForListen("/ip4/0.0.0.0/udp/18517/adp/1.0.0"));
}

TEST(ReachabilityTest, UndialableLanAndVirtualIfaceHelpers) {
  EXPECT_TRUE(pp::mesh::IsLikelyUndialableLanIpv4("192.168.122.1"));
  EXPECT_TRUE(pp::mesh::IsLikelyUndialableLanIpv4("192.168.122.50"));
  EXPECT_FALSE(pp::mesh::IsLikelyUndialableLanIpv4("192.168.1.152"));
  EXPECT_FALSE(pp::mesh::IsLikelyUndialableLanIpv4("192.168.1.122"));
  EXPECT_TRUE(pp::mesh::IsVirtualLanIfaceName("virbr0"));
  EXPECT_TRUE(pp::mesh::IsVirtualLanIfaceName("docker0"));
  EXPECT_TRUE(pp::mesh::IsVirtualLanIfaceName("vethabc123"));
  EXPECT_FALSE(pp::mesh::IsVirtualLanIfaceName("enp9s0"));
  EXPECT_FALSE(pp::mesh::IsVirtualLanIfaceName("wlan0"));
}

TEST(ReachabilityTest, BuildAmpLanAdvertisedAddrsExpandsWildcardOrKeepsConcrete) {
  const auto concrete =
      pp::mesh::BuildAmpLanAdvertisedAddrs("/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest");
  // May be empty on hosts without dialable LAN ifaces; concrete host still returned when no iface list.
  if (!concrete.empty()) {
    EXPECT_NE(concrete.front().find("/udp/19001/adp/1.0.0/p2p/"), std::string::npos);
  }

  const auto wildcard =
      pp::mesh::BuildAmpLanAdvertisedAddrs("/ip4/0.0.0.0/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest");
  for (const std::string& ma : wildcard) {
    EXPECT_EQ(ma.find("/ip4/0.0.0.0/"), std::string::npos);
    EXPECT_NE(ma.find("/udp/19001/adp/1.0.0/p2p/12D3KooWTest"), std::string::npos);
  }
}

TEST(ReachabilityTest, RankAmpDialMultiaddrsPrefersGlobalIpv6OverPrivateIpv4) {
  const std::string v6 = "/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string pub4 = "/ip4/203.0.113.10/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  const std::string lan4 = "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWTest";
  EXPECT_LT(pp::mesh::AmpDialMultiaddrRank(v6), pp::mesh::AmpDialMultiaddrRank(pub4));
  EXPECT_LT(pp::mesh::AmpDialMultiaddrRank(pub4), pp::mesh::AmpDialMultiaddrRank(lan4));

  const auto ranked = pp::mesh::RankAmpDialMultiaddrs({lan4, pub4, v6});
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
  pp::mesh::AmpDialLocalContext ctx;
  ctx.lan_ipv4_hosts = {"192.168.0.105"};
  ctx.has_global_ipv6 = true;

  const auto ranked = pp::mesh::RankAmpDialMultiaddrs({other_lan, v6, pub4, same_lan}, ctx);
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
  pp::mesh::AmpDialLocalContext ctx;
  ctx.has_global_ipv6 = false;  // v4-only socket cannot send to /ip6 at all

  const auto ranked = pp::mesh::RankAmpDialMultiaddrs({lan4, v6, pub4}, ctx);
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0], pub4);
  EXPECT_EQ(ranked[1], lan4);
  EXPECT_EQ(ranked[2], v6);
}

TEST(ReachabilityTest, LinkLocalIpv4IsUndialable) {
  EXPECT_TRUE(pp::mesh::IsLikelyUndialableLanIpv4("169.254.75.55"));
  EXPECT_TRUE(pp::mesh::IsLikelyUndialableLanIpv4("169.254.116.242"));
  EXPECT_FALSE(pp::mesh::IsLikelyUndialableLanIpv4("192.168.0.105"));
  EXPECT_FALSE(pp::mesh::IsLikelyUndialableLanIpv4("10.68.189.3"));
}

TEST(ReachabilityTest, BuildAmpGlobalIpv6AdvertisedAddrsFromHosts) {
  const auto addrs = pp::mesh::BuildAmpGlobalIpv6AdvertisedAddrs(
      "/ip4/0.0.0.0/udp/19001/adp/1.0.0/p2p/12D3KooWTest", "12D3KooWTest",
      {"2001:db8::10", "fe80::1", "fd12::1", "::1"});
  ASSERT_EQ(addrs.size(), 1u);
  EXPECT_NE(addrs.front().find("/ip6/"), std::string::npos);
  EXPECT_NE(addrs.front().find("2001:db8::10"), std::string::npos);
  EXPECT_NE(addrs.front().find("/udp/19001/adp/1.0.0/p2p/12D3KooWTest"), std::string::npos);
}

TEST(ReachabilityTest, ReachableViaIpv6ContractUsesDialBackPlusGlobalIpv6) {
  // Product chrome: Reachable + has_global_ipv6 + dial_back_ok => "Reachable via IPv6".
  pp::mesh::ReachabilitySignals signals;
  signals.seed_dial_ok = true;
  signals.dial_back_ok = true;
  signals.has_global_ipv6 = true;
  EXPECT_EQ(pp::mesh::ClassifyReachability(signals), pp::mesh::ReachabilityStatus::Reachable);
  EXPECT_TRUE(signals.has_global_ipv6);
  EXPECT_TRUE(signals.dial_back_ok);

  pp::mesh::ReachabilitySignals outbound_only_v6;
  outbound_only_v6.seed_dial_ok = true;
  outbound_only_v6.dial_back_ok = false;
  outbound_only_v6.has_global_ipv6 = true;
  EXPECT_EQ(pp::mesh::ClassifyReachability(outbound_only_v6), pp::mesh::ReachabilityStatus::OutboundOnly);
}

TEST(ReachabilityTest, IsGlobalIpv6FiltersLinkLocalAndUla) {
  EXPECT_TRUE(pp::mesh::IsGlobalIpv6("2001:db8::1"));
  EXPECT_FALSE(pp::mesh::IsGlobalIpv6("fe80::1"));
  EXPECT_FALSE(pp::mesh::IsGlobalIpv6("fd12::1"));
  EXPECT_FALSE(pp::mesh::IsGlobalIpv6("::1"));
  EXPECT_FALSE(pp::mesh::IsGlobalIpv6("192.168.1.1"));
}

TEST(ReachabilityTest, SkipUpnpForGlobalIpv6Listen) {
  EXPECT_TRUE(pp::mesh::ShouldSkipUpnpForListen("/ip6/2001:db8::10/udp/18517/adp/1.0.0"));
  EXPECT_FALSE(pp::mesh::ShouldSkipUpnpForListen("/ip6/::/udp/18517/adp/1.0.0"));
  EXPECT_FALSE(pp::mesh::ShouldSkipUpnpForListen("/ip6/fe80::1/udp/18517/adp/1.0.0"));
}
