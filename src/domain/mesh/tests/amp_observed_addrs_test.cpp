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

} // namespace
} // namespace pbr
