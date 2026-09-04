#include "domain/mesh/reachability/PunchLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PunchLogicTest, ConnectRoundTrip) {
  PunchConnectRequest req;
  req.target_peer_id = "12D3KooWTarget";
  req.addrs = {"/ip4/192.168.1.10/udp/19001/adp/1.0.0/p2p/12D3KooWSelf"};
  req.window_ms = 1500;
  req.reason = "cold";

  const std::string json = EncodePunchConnect(req);
  auto root = TryParseObject(json);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(PunchOp(*root).value_or(""), "connect");
  auto decoded = DecodePunchConnect(*root);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->target_peer_id, req.target_peer_id);
  ASSERT_EQ(decoded->addrs.size(), 1u);
  EXPECT_EQ(decoded->addrs[0], req.addrs[0]);
  EXPECT_EQ(decoded->window_ms, 1500);
}

TEST(PunchLogicTest, SanitizeDropsNonAdpAndCaps) {
  std::vector<std::string> in = {
      "/ip4/1.2.3.4/udp/9/adp/1.0.0/p2p/12D3KooWA",
      "/ip4/1.2.3.4/tcp/9",
      "/ip4/1.2.3.4/udp/9/adp/1.0.0/p2p/12D3KooWA",
      "not-a-multiaddr",
  };
  for (int i = 0; i < 10; ++i) {
    in.push_back("/ip4/9.9.9." + std::to_string(i) + "/udp/9/adp/1.0.0/p2p/12D3KooWB");
  }
  const auto out = SanitizePunchAddrs(in, 8);
  EXPECT_LE(out.size(), 8u);
  for (const std::string& ma : out) {
    EXPECT_NE(ma.find("/adp/1.0.0/"), std::string::npos);
  }
}

TEST(PunchLogicTest, WindowOpenBounds) {
  EXPECT_TRUE(PunchWindowOpen(1000, 500, 1200));
  EXPECT_FALSE(PunchWindowOpen(1000, 500, 1600));
  EXPECT_FALSE(PunchWindowOpen(1000, 0, 1000));
}

TEST(PunchLogicTest, SyncAndResultRoundTrip) {
  PunchSync sync;
  sync.epoch_id = "ep-1";
  sync.peer_addrs = {"/ip4/10.0.0.2/udp/7/adp/1.0.0/p2p/12D3KooWC"};
  sync.window_ms = 2000;
  auto sync_root = TryParseObject(EncodePunchSync(sync));
  ASSERT_TRUE(sync_root);
  auto sync_decoded = DecodePunchSync(*sync_root);
  ASSERT_TRUE(sync_decoded);
  EXPECT_EQ(sync_decoded->epoch_id, "ep-1");

  PunchResult result;
  result.epoch_id = "ep-1";
  result.ok = true;
  result.winner_multiaddr = sync.peer_addrs.front();
  auto result_root = TryParseObject(EncodePunchResult(result));
  ASSERT_TRUE(result_root);
  auto result_decoded = DecodePunchResult(*result_root);
  ASSERT_TRUE(result_decoded);
  EXPECT_TRUE(result_decoded->ok);
  EXPECT_EQ(result_decoded->winner_multiaddr, result.winner_multiaddr);
}

} // namespace
} // namespace pbr
