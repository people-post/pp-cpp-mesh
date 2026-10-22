#include "mesh/dht/AmpDhtProtocol.h"
#include "mesh/dht/DhtRecordCodec.h"

#include "common/SettledWait.h"
#include "mesh/testing/mesh_test_harness.h"
#include "mesh/testing/mesh_triple_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
namespace pp::mesh {
namespace {

TEST(AmpDhtProtocolTest, FindPeerReturnsBootstrapRecord) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  AmpDhtProtocol seed(*harness->runtime_b);
  AmpDhtProtocol client(*harness->runtime_a);

  AmpDhtProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = harness->peer_id_b;
  seed_cfg.listen_multiaddrs = {harness->ma_b};
  seed_cfg.device_signing_secret = harness->bob.ml_dsa_secret_key;
  seed_cfg.device_signing_public = harness->bob.ml_dsa_public_key;
  seed_cfg.participate = true;
  seed_cfg.publish_media_relay = true;
  seed_cfg.publish_circuit_relay = true;
  seed.Configure(seed_cfg);

  AmpDhtProtocolConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.listen_multiaddrs = {harness->ma_a};
  client_cfg.device_signing_secret = harness->alice.ml_dsa_secret_key;
  client_cfg.device_signing_public = harness->alice.ml_dsa_public_key;
  client_cfg.query_peer_keys = {"seed"};
  client_cfg.participate = false;
  client.Configure(client_cfg);

  seed.Start();
  client.Start();
  seed.Tick();

  SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait;
  client.FindPeer(harness->peer_id_b,
                  [&wait](AmpDhtProtocol::FindPeerRoe result) { wait.Finish(std::move(result)); });
  harness->PumpUntil([&]() { return wait.IsSettled(); }, 1000);

  auto found =
      wait.Wait(std::chrono::seconds(5), AmpDhtProtocol::Failure::Of(AmpDhtProtocol::Err::Timeout, "find_peer timed out"));
  ASSERT_TRUE(static_cast<bool>(found)) << found.error().message;
  EXPECT_EQ(found->peer_id, harness->peer_id_b);
  EXPECT_FALSE(found->record.multiaddrs.empty());
  ASSERT_TRUE(found->record.capabilities.has_value());
  EXPECT_TRUE(found->record.capabilities->media_relay);
  EXPECT_TRUE(found->record.capabilities->circuit_relay);
  EXPECT_TRUE(VerifyPeerRoutingRecordStandalone(found->record)) << "carries its key: checkable by anyone";

  client.Stop();
  seed.Stop();
}

// A lookup whose reply channel ends without an answer (read timeout here) settles with an error.
// It used to hang: its concurrency slot never came back, and after max_concurrent_lookups (4)
// such lookups every find_peer failed with ConcurrencyLimit for the process's lifetime.
TEST(AmpDhtProtocolTest, LookupToASilentPeerSettlesAndFreesItsSlot) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));

  // The "seed" accepts DHT channels and never answers.
  std::vector<std::shared_ptr<pp::amp::ChannelSession>> silent;
  harness->runtime_b->SetProtocolHandler(
      kDhtProtocolId, [&](pp::amp::LinkHandle, const std::string& remote_peer_id, const uint32_t channel_id) {
        silent.push_back(harness->mgr_b().BindChannel(remote_peer_id, channel_id, pp::amp::ControlJsonChannelPolicy(),
                                                      [](Roe<std::vector<uint8_t>>) { return true; }));
      });

  AmpDhtProtocol client(*harness->runtime_a);
  AmpDhtProtocolConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.listen_multiaddrs = {harness->ma_a};
  client_cfg.device_signing_secret = harness->alice.ml_dsa_secret_key;
  client_cfg.device_signing_public = harness->alice.ml_dsa_public_key;
  client_cfg.query_peer_keys = {"seed"};
  client.Configure(client_cfg);
  client.Start();

  for (int i = 0; i < 6; ++i) {  // more than the default 4 concurrent lookups
    SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait;
    client.FindPeer("QmSomeoneElse",
                    [&wait](AmpDhtProtocol::FindPeerRoe result) { wait.Finish(std::move(result)); });
    harness->PumpUntil([&]() { return wait.IsSettled(); }, 30000);
    ASSERT_TRUE(wait.IsSettled()) << "lookup " << i << " never settled";
    auto found = wait.Wait(std::chrono::seconds(1),
                           AmpDhtProtocol::Failure::Of(AmpDhtProtocol::Err::Timeout, "unsettled"));
    ASSERT_FALSE(static_cast<bool>(found));
    EXPECT_NE(found.error().GetCode(), AmpDhtProtocol::Err::ConcurrencyLimit) << "lookup " << i;
  }
  client.Stop();
  harness->runtime_b->SetProtocolHandler(kDhtProtocolId, {});
}

// The routing table admits participants: a node whose store was accepted, and a node that
// answered as a participant (with the dial address from its own record). docs/DHT.md § 4.2.
TEST(AmpDhtProtocolTest, ParticipantsEnterTheRoutingTable) {
  auto created = pp::mesh::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto h = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(h->mgr_a().RegisterEndpoint(h->peer_id_r, h->ma_r)));
  ASSERT_TRUE(static_cast<bool>(h->mgr_b().RegisterEndpoint(h->peer_id_r, h->ma_r)));

  AmpDhtProtocol node_a(*h->runtime_a);
  AmpDhtProtocol node_r(*h->runtime_r);
  AmpDhtProtocol node_b(*h->runtime_b);
  const auto configure = [](AmpDhtProtocol& node, const std::string& peer_id, const std::string& ma,
                            const pp::amp::MshIdentity& id, std::vector<std::string> query_peers) {
    AmpDhtProtocolConfig cfg;
    cfg.local_peer_id = peer_id;
    cfg.listen_multiaddrs = {ma};
    cfg.device_signing_secret = id.ml_dsa_secret_key;
    cfg.device_signing_public = id.ml_dsa_public_key;
    cfg.query_peer_keys = std::move(query_peers);
    cfg.participate = true;
    node.Configure(cfg);
    node.Start();
  };
  configure(node_r, h->peer_id_r, h->ma_r, h->relay, {});
  configure(node_a, h->peer_id_a, h->ma_a, h->alice, {h->peer_id_r});
  configure(node_b, h->peer_id_b, h->ma_b, h->bob, {h->peer_id_r});

  // Each publishes to R (a store) and looks R up (R answers with its own record).
  h->PumpUntil([&]() {
    return node_r.RoutingTable().Contains(h->peer_id_a) && node_r.RoutingTable().Contains(h->peer_id_b) &&
           node_a.RoutingTable().Contains(h->peer_id_r) && node_b.RoutingTable().Contains(h->peer_id_r);
  }, 4000);
  EXPECT_TRUE(node_r.RoutingTable().Contains(h->peer_id_a));
  EXPECT_TRUE(node_r.RoutingTable().Contains(h->peer_id_b));
  ASSERT_TRUE(node_a.RoutingTable().Contains(h->peer_id_r));
  EXPECT_EQ(node_a.RoutingTable().Find(h->peer_id_r)->multiaddr, h->ma_r) << "from R's own record";
  EXPECT_FALSE(node_a.RoutingTable().Contains(h->peer_id_b)) << "never heard from B";
  EXPECT_EQ(node_r.Stats().routing_contacts, 2u);

  node_a.Stop();
  node_b.Stop();
  node_r.Stop();
}

// A lookup follows closer_peers over the wire: R does not have the target but knows B, so A asks B
// too (dialling it at the hinted address) — and then has B in its own table. A miss is not
// repeated within the negative-cache window.
TEST(AmpDhtProtocolTest, ALookupFollowsCloserPeersAndCachesTheMiss) {
  auto created = pp::mesh::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto h = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(h->mgr_a().RegisterEndpoint(h->peer_id_r, h->ma_r)));
  ASSERT_TRUE(static_cast<bool>(h->mgr_b().RegisterEndpoint(h->peer_id_r, h->ma_r)));

  AmpDhtProtocol node_a(*h->runtime_a);
  AmpDhtProtocol node_r(*h->runtime_r);
  AmpDhtProtocol node_b(*h->runtime_b);
  const auto configure = [](AmpDhtProtocol& node, const std::string& peer_id, const std::string& ma,
                            const pp::amp::MshIdentity& id, std::vector<std::string> query_peers) {
    AmpDhtProtocolConfig cfg;
    cfg.local_peer_id = peer_id;
    cfg.listen_multiaddrs = {ma};
    cfg.device_signing_secret = id.ml_dsa_secret_key;
    cfg.device_signing_public = id.ml_dsa_public_key;
    cfg.query_peer_keys = std::move(query_peers);
    cfg.participate = true;
    node.Configure(cfg);
    node.Start();
  };
  configure(node_r, h->peer_id_r, h->ma_r, h->relay, {});
  configure(node_a, h->peer_id_a, h->ma_a, h->alice, {h->peer_id_r});
  configure(node_b, h->peer_id_b, h->ma_b, h->bob, {h->peer_id_r});
  h->PumpUntil([&]() {
    return node_r.RoutingTable().Contains(h->peer_id_a) && node_r.RoutingTable().Contains(h->peer_id_b) &&
           node_a.RoutingTable().Contains(h->peer_id_r);
  }, 4000);
  ASSERT_TRUE(node_r.RoutingTable().Contains(h->peer_id_b));
  ASSERT_FALSE(node_a.RoutingTable().Contains(h->peer_id_b));

  const std::string nobody = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
  const auto find = [&](const std::string& target) {
    SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait;
    node_a.FindPeer(target, [&wait](AmpDhtProtocol::FindPeerRoe result) { wait.Finish(std::move(result)); });
    h->PumpUntil([&]() { return wait.IsSettled(); }, 4000);
    return wait.Wait(std::chrono::seconds(5),
                     AmpDhtProtocol::Failure::Of(AmpDhtProtocol::Err::Timeout, "find_peer timed out"));
  };
  auto missed = find(nobody);
  ASSERT_FALSE(static_cast<bool>(missed));
  EXPECT_EQ(missed.error().GetCode(), AmpDhtProtocol::Err::NotFound) << missed.error().message;
  EXPECT_TRUE(node_a.RoutingTable().Contains(h->peer_id_b)) << "asked B from R's hint, and B answered";

  const uint64_t issued = node_a.Stats().find_peer_issued;
  auto again = find(nobody);
  ASSERT_FALSE(static_cast<bool>(again));
  EXPECT_EQ(again.error().GetCode(), AmpDhtProtocol::Err::NotFound);
  EXPECT_EQ(node_a.Stats().find_peer_issued, issued) << "the miss is cached: nothing asked";

  node_a.Stop();
  node_b.Stop();
  node_r.Stop();
}

// Publishing (docs/DHT.md § 4.5): B stores its record at the closest nodes a walk to its key
// finds — A too, though A is not B's query peer and never looked B up. A closed gate publishes
// nothing; new addresses publish again at once.
TEST(AmpDhtProtocolTest, ARecordIsPublishedAtTheClosestBehindTheGate) {
  auto created = pp::mesh::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto h = std::move(*created);
  h->ep_a->SetAcceptEnabled(true);  // A takes part: it is dialled at its published address
  ASSERT_TRUE(static_cast<bool>(h->mgr_a().RegisterEndpoint(h->peer_id_r, h->ma_r)));
  ASSERT_TRUE(static_cast<bool>(h->mgr_b().RegisterEndpoint(h->peer_id_r, h->ma_r)));

  AmpDhtProtocol node_a(*h->runtime_a);
  AmpDhtProtocol node_r(*h->runtime_r);
  AmpDhtProtocol node_b(*h->runtime_b);
  const auto config_for = [](const std::string& peer_id, const std::string& ma, const pp::amp::MshIdentity& id,
                             std::vector<std::string> query_peers) {
    AmpDhtProtocolConfig cfg;
    cfg.local_peer_id = peer_id;
    cfg.listen_multiaddrs = {ma};
    cfg.device_signing_secret = id.ml_dsa_secret_key;
    cfg.device_signing_public = id.ml_dsa_public_key;
    cfg.query_peer_keys = std::move(query_peers);
    cfg.participate = true;
    return cfg;
  };
  node_r.Configure(config_for(h->peer_id_r, h->ma_r, h->relay, {}));
  node_r.Start();
  node_a.Configure(config_for(h->peer_id_a, h->ma_a, h->alice, {h->peer_id_r}));
  node_a.Start();
  h->PumpUntil([&]() { return node_r.RoutingTable().Contains(h->peer_id_a); }, 4000);
  ASSERT_TRUE(node_r.RoutingTable().Contains(h->peer_id_a));

  // B's gate is closed (as if Blocked): nothing goes out.
  std::atomic<bool> open{false};
  std::vector<std::string> b_addrs = {h->ma_b};
  std::mutex b_addrs_mu;
  AmpDhtProtocolConfig cfg_b = config_for(h->peer_id_b, h->ma_b, h->bob, {h->peer_id_r});
  cfg_b.may_publish = [&open]() { return open.load(); };
  cfg_b.current_listen_multiaddrs = [&]() {
    std::lock_guard lock(b_addrs_mu);
    return b_addrs;
  };
  node_b.Configure(cfg_b);
  node_b.Start();
  h->PumpUntil([]() { return false; }, 200);
  EXPECT_EQ(node_b.Stats().self_publishes, 0u);
  EXPECT_FALSE(node_r.LocalRecord(h->peer_id_b).has_value());

  open = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(DhtClient::kGateCheckMs + 50));
  node_b.Tick();
  h->PumpUntil([&]() { return node_a.LocalRecord(h->peer_id_b).has_value() && node_r.LocalRecord(h->peer_id_b).has_value(); },
               4000);
  ASSERT_TRUE(node_a.LocalRecord(h->peer_id_b).has_value()) << "stored at A, found by B's walk via R";
  ASSERT_TRUE(node_r.LocalRecord(h->peer_id_b).has_value());
  EXPECT_EQ(node_b.Stats().last_publish_targets, 2u);
  const int64_t first_seq = node_r.LocalRecord(h->peer_id_b)->seq;

  // New addresses: published again without waiting for ttl / 2.
  const std::string moved_to = "/ip4/198.51.100.7/udp/9/adp/1.0.0/p2p/" + h->peer_id_b;
  {
    std::lock_guard lock(b_addrs_mu);
    b_addrs = {h->ma_b, moved_to};
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(DhtClient::kGateCheckMs + 50));
  node_b.Tick();
  h->PumpUntil([&]() { return node_r.LocalRecord(h->peer_id_b)->seq > first_seq; }, 4000);
  const auto republished = node_r.LocalRecord(h->peer_id_b);
  ASSERT_TRUE(republished.has_value());
  EXPECT_GT(republished->seq, first_seq);
  EXPECT_EQ(republished->multiaddrs.size(), 2u);

  node_a.Stop();
  node_b.Stop();
  node_r.Stop();
}

// Liveness comes from the reach record (docs/DHT.md D5): a peer it holds down is not dialled for
// a walk nor stored at, and leaves the routing table once its link is down too.
TEST(AmpDhtProtocolTest, PeersTheReachRecordHoldsDownAreSkippedAndDropped) {
  auto created = pp::mesh::test::AmpMeshTripleHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto h = std::move(*created);
  h->ep_a->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(h->mgr_a().RegisterEndpoint(h->peer_id_r, h->ma_r)));
  ASSERT_TRUE(static_cast<bool>(h->mgr_b().RegisterEndpoint(h->peer_id_r, h->ma_r)));

  AmpDhtProtocol node_a(*h->runtime_a);
  AmpDhtProtocol node_r(*h->runtime_r);
  AmpDhtProtocol node_b(*h->runtime_b);
  const auto config_for = [](const std::string& peer_id, const std::string& ma, const pp::amp::MshIdentity& id,
                             std::vector<std::string> query_peers) {
    AmpDhtProtocolConfig cfg;
    cfg.local_peer_id = peer_id;
    cfg.listen_multiaddrs = {ma};
    cfg.device_signing_secret = id.ml_dsa_secret_key;
    cfg.device_signing_public = id.ml_dsa_public_key;
    cfg.query_peer_keys = std::move(query_peers);
    cfg.participate = true;
    return cfg;
  };
  std::atomic<bool> r_holds_a_down{false};
  AmpDhtProtocolConfig cfg_r = config_for(h->peer_id_r, h->ma_r, h->relay, {});
  cfg_r.reach_held_down = [&](const std::string& peer_id) { return r_holds_a_down && peer_id == h->peer_id_a; };
  node_r.Configure(cfg_r);
  node_r.Start();
  node_a.Configure(config_for(h->peer_id_a, h->ma_a, h->alice, {h->peer_id_r}));
  node_a.Start();
  h->PumpUntil([&]() { return node_r.RoutingTable().Contains(h->peer_id_a); }, 4000);
  ASSERT_TRUE(node_r.RoutingTable().Contains(h->peer_id_a));

  // B's record holds A down: R's hint to A is not followed, and A is not stored at.
  AmpDhtProtocolConfig cfg_b = config_for(h->peer_id_b, h->ma_b, h->bob, {h->peer_id_r});
  cfg_b.reach_held_down = [&](const std::string& peer_id) { return peer_id == h->peer_id_a; };
  node_b.Configure(cfg_b);
  node_b.Start();
  h->PumpUntil([&]() { return node_b.Stats().self_publishes > 0 && node_r.LocalRecord(h->peer_id_b).has_value(); },
               4000);
  ASSERT_EQ(node_b.Stats().self_publishes, 1u);
  EXPECT_EQ(node_b.Stats().last_publish_targets, 1u) << "R only";
  h->PumpUntil([]() { return false; }, 200);
  EXPECT_FALSE(node_a.LocalRecord(h->peer_id_b).has_value());
  EXPECT_FALSE(h->mgr_b().IsConnectedToPeerId(h->peer_id_a)) << "never dialled";

  // R: held down but linked — kept; held down and unlinked — dropped.
  r_holds_a_down = true;
  node_r.SweepHeldDown();
  EXPECT_TRUE(node_r.RoutingTable().Contains(h->peer_id_a)) << "its link is up";
  h->mgr_r().RequestDropLink(h->peer_id_a);
  h->PumpUntil([&]() { return !h->mgr_r().IsConnectedToPeerId(h->peer_id_a); }, 4000);
  ASSERT_FALSE(h->mgr_r().IsConnectedToPeerId(h->peer_id_a));
  node_r.SweepHeldDown();
  EXPECT_FALSE(node_r.RoutingTable().Contains(h->peer_id_a));

  node_a.Stop();
  node_b.Stop();
  node_r.Stop();
}

/** Lab acceptance: two participating Nodes discover each other's ADP addrs (no Brief HTTP). */
TEST(AmpDhtProtocolTest, MutualDiscoverViaStoreAndWarmFindPeer) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("b", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("a", harness->ma_a)));

  AmpDhtProtocol node_a(*harness->runtime_a);
  AmpDhtProtocol node_b(*harness->runtime_b);

  AmpDhtProtocolConfig cfg_a;
  cfg_a.local_peer_id = harness->peer_id_a;
  cfg_a.listen_multiaddrs = {harness->ma_a};
  cfg_a.device_signing_secret = harness->alice.ml_dsa_secret_key;
  cfg_a.device_signing_public = harness->alice.ml_dsa_public_key;
  cfg_a.query_peer_keys = {"b"};
  cfg_a.participate = true;
  node_a.Configure(cfg_a);

  AmpDhtProtocolConfig cfg_b;
  cfg_b.local_peer_id = harness->peer_id_b;
  cfg_b.listen_multiaddrs = {harness->ma_b};
  cfg_b.device_signing_secret = harness->bob.ml_dsa_secret_key;
  cfg_b.device_signing_public = harness->bob.ml_dsa_public_key;
  cfg_b.query_peer_keys = {"a"};
  cfg_b.participate = true;
  node_b.Configure(cfg_b);

  node_a.Start();
  node_b.Start();

  // Sequential publish + FindPeer (avoids concurrent dual-dial Store/FindPeer races).
  node_a.Tick();
  SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait_a;
  node_a.FindPeer(harness->peer_id_b,
                  [&wait_a](AmpDhtProtocol::FindPeerRoe result) { wait_a.Finish(std::move(result)); });
  harness->PumpUntil([&]() { return wait_a.IsSettled(); }, 2000);
  auto found_b = wait_a.Wait(std::chrono::seconds(5), AmpDhtProtocol::Failure::Of(
                                                          AmpDhtProtocol::Err::Timeout, "find_peer A→B timed out"));
  ASSERT_TRUE(static_cast<bool>(found_b)) << found_b.error().message;

  node_b.Tick();
  SettledWait<DhtFindPeerResult, AmpDhtProtocol::Failure> wait_b;
  node_b.FindPeer(harness->peer_id_a,
                  [&wait_b](AmpDhtProtocol::FindPeerRoe result) { wait_b.Finish(std::move(result)); });
  harness->PumpUntil([&]() { return wait_b.IsSettled(); }, 2000);
  auto found_a = wait_b.Wait(std::chrono::seconds(5), AmpDhtProtocol::Failure::Of(
                                                          AmpDhtProtocol::Err::Timeout, "find_peer B→A timed out"));
  ASSERT_TRUE(static_cast<bool>(found_a)) << found_a.error().message;

  auto a_has_b = node_a.LocalRecord(harness->peer_id_b);
  auto b_has_a = node_b.LocalRecord(harness->peer_id_a);
  ASSERT_TRUE(a_has_b.has_value()) << "node A missing B's DHT record";
  ASSERT_TRUE(b_has_a.has_value()) << "node B missing A's DHT record";
  EXPECT_FALSE(a_has_b->multiaddrs.empty());
  EXPECT_FALSE(b_has_a->multiaddrs.empty());
  EXPECT_NE(a_has_b->multiaddrs.front().find("/adp/"), std::string::npos);
  EXPECT_NE(b_has_a->multiaddrs.front().find("/adp/"), std::string::npos);

  node_a.Stop();
  node_b.Stop();
}

TEST(AmpDhtProtocolTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  EXPECT_EQ(AmpDhtProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow")).GetCode(),
            AmpDhtProtocol::Err::Timeout);
  EXPECT_EQ(AmpDhtProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux")).GetCode(),
            AmpDhtProtocol::Err::ChannelFailed);
  const auto wrapped =
      AmpDhtProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::AssociationNotReady, "not ready"));
  EXPECT_EQ(wrapped.GetCode(), AmpDhtProtocol::Err::LinkFailed);
  EXPECT_NE(wrapped.message.find("[link:"), std::string::npos);
}

} // namespace
} // namespace pp::mesh
