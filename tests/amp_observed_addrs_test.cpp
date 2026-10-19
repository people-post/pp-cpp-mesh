#include "domain/mesh/reachability/AmpObservedAddrs.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AmpObservedAddrsTest, MergesLanUpnpAndDialBackWithoutWildcard) {
  ReachabilitySnapshot snap;
  snap.signals.upnp_mapped = true;
  snap.signals.upnp_external_ip = "203.0.113.10";
  snap.signals.upnp_external_port = 19001;
  snap.signals.dial_back_ok = true;
  snap.signals.dial_back_dialed =
      "/ip4/198.51.100.20/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer";

  const auto set = CollectAmpObservedAddrs(
      "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer", "12D3KooWObservedPeer", snap);
  const auto merged = set.MergedForAdvertise();
  ASSERT_FALSE(merged.empty());
  for (const std::string& ma : merged) {
    EXPECT_EQ(ma.find("/ip4/0.0.0.0/"), std::string::npos);
    EXPECT_EQ(ma.find("/ip4/127.0.0.1/"), std::string::npos);
    EXPECT_NE(ma.find("/adp/1.0.0/p2p/12D3KooWObservedPeer"), std::string::npos);
  }
  bool saw_dial_back = false;
  for (const std::string& ma : merged) {
    if (ma.find("198.51.100.20") != std::string::npos) {
      saw_dial_back = true;
    }
  }
  EXPECT_TRUE(saw_dial_back);
}

TEST(AmpObservedAddrsTest, SkipsUnusableDialBack) {
  ReachabilitySnapshot snap;
  snap.signals.dial_back_ok = true;
  snap.signals.dial_back_dialed = "/ip4/127.0.0.1/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer";
  const auto set = CollectAmpObservedAddrs(
      "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer", "12D3KooWObservedPeer", snap);
  for (const std::string& ma : set.MergedForPunch()) {
    EXPECT_EQ(ma.find("127.0.0.1"), std::string::npos);
  }
}


TEST(AmpObservedAddrsTest, PrefersSeedObservedReflexiveIpv4OverLanOnly) {
  ReachabilitySnapshot snap;
  // Probe dial of LAN targets failed, but seed reported reflexive public IPv4 (B26).
  snap.signals.dial_back_ok = false;
  snap.signals.dial_back_error = "dial timed out";
  snap.signals.dial_back_observed =
      "/ip4/203.0.113.77/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer";

  const auto set = CollectAmpObservedAddrs(
      "/ip4/192.168.0.105/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer", "12D3KooWObservedPeer", snap);
  const auto merged = set.MergedForAdvertise();
  ASSERT_FALSE(merged.empty());
  size_t reflexive_i = merged.size();
  size_t lan_i = merged.size();
  for (size_t i = 0; i < merged.size(); ++i) {
    if (merged[i].find("203.0.113.77") != std::string::npos) {
      reflexive_i = i;
    }
    if (merged[i].find("192.168.0.105") != std::string::npos) {
      lan_i = i;
    }
  }
  EXPECT_LT(reflexive_i, merged.size()) << "reflexive public IPv4 must be advertised";
  if (lan_i < merged.size()) {
    EXPECT_LT(reflexive_i, lan_i) << "public reflexive must rank ahead of LAN";
  }
}

TEST(AmpObservedAddrsTest, PrefersGlobalIpv6BeforePrivateLan) {
  ReachabilitySnapshot snap;
  // Inject via listen list directly: CollectAmpObservedAddrs enumerates host ifaces;
  // unit-test ranking through MergedForAdvertise on a crafted set.
  AmpObservedAddrSet set;
  set.listen = {
      "/ip4/192.168.1.50/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer",
      "/ip6/2001:db8::55/udp/19001/adp/1.0.0/p2p/12D3KooWObservedPeer",
  };
  const auto merged = set.MergedForAdvertise();
  ASSERT_GE(merged.size(), 2u);
  EXPECT_NE(merged.front().find("/ip6/2001:db8::55/"), std::string::npos)
      << "global /ip6 must precede private /ip4 for PreferredMultiaddr ingest";
}

TEST(AmpObservedAddrsTest, IsUsableAdpListenRejectsWildcardAndLinkLocal) {
  EXPECT_FALSE(IsUsableAdpListen("/ip4/0.0.0.0/udp/48599/adp/1.0.0/p2p/12D3KooWPeer"));
  EXPECT_FALSE(IsUsableAdpListen("/ip6/::/udp/48599/adp/1.0.0/p2p/12D3KooWPeer"));
  EXPECT_FALSE(IsUsableAdpListen("/ip4/127.0.0.1/udp/48599/adp/1.0.0/p2p/12D3KooWPeer"));
  EXPECT_FALSE(IsUsableAdpListen("/ip4/169.254.75.55/udp/48599/adp/1.0.0/p2p/12D3KooWPeer"));
  EXPECT_FALSE(IsUsableAdpListen("/ip4/192.168.122.1/udp/48599/adp/1.0.0/p2p/12D3KooWPeer"));
  EXPECT_TRUE(IsUsableAdpListen("/ip4/192.168.0.105/udp/60581/adp/1.0.0/p2p/12D3KooWPeer"));
}

} // namespace
} // namespace pbr
