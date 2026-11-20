#include "mesh/discovery/AmpDirectoryProtocol.h"

#include "mesh/dht/DhtRecordCodec.h"
#include "mesh/discovery/MeshNodeHitCodec.h"

#include "common/SettledWait.h"
#include "crypto/AlgId.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"
#include "mesh/testing/mesh_test_harness.h"

#include <gtest/gtest.h>
namespace pp::mesh {
namespace {

TEST(AmpDirectoryProtocolTest, ListMeshNodesReturnsSeedSnapshot) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));

  auto pump = [&]() { harness->PumpBoth(); };

  AmpDirectoryProtocol seed(*harness->runtime_b, pump);
  AmpDirectoryProtocol client(*harness->runtime_a, pump);

  AmpDirectoryProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = harness->peer_id_b;
  seed.Configure(seed_cfg);

  MeshNodeHit published;
  published.account_id = "account:seed";
  published.nickname = "SeedNode";
  published.entity_kind = "mesh_node";
  published.seq = 7;
  published.capabilities.circuit_relay = true;
  published.capabilities.media_relay = true;
  published.capabilities.dht = true;
  published.capabilities.ledger_gateway = true;
  // The account's keys as a key card carries them (typed blobs, the KEM binding v2): carried, not verified.
  published.signing_public_key_b64 = PeerRoutingPublicKeyB64(harness->bob.ml_dsa_public_key).valueOr("");
  pp::ByteVector kem(1 + 1184, 0x11);
  kem[0] = pp::kKemAlgMlKem768;
  published.kem_public_key_b64 = pp::Base64Encode(kem);
  pp::ByteVector binding{kKemBindingVersion, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0c, 0xee};
  binding.push_back(pp::kSignatureAlgMlDsa65);
  binding.insert(binding.end(), pp::kMlDsa65SignatureBytes, 0x22);
  published.kem_binding_b64 = pp::Base64Encode(binding);
  ASSERT_EQ(KemBindingShape(*published.kem_binding_b64), DirectoryKeyShape::Ok);
  DirectoryEndpoint ep;
  ep.peer_id = harness->peer_id_b;
  ep.multiaddrs = {harness->ma_b};
  published.endpoints.push_back(std::move(ep));
  seed.SetNodesSnapshot({published});

  AmpDirectoryProtocolConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.query_peer_keys = {"seed"};
  client.Configure(client_cfg);

  seed.Start();
  client.Start();

  auto listed = client.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(listed)) << listed.error().message;
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ(listed->front().account_id, "account:seed");
  EXPECT_EQ(listed->front().seq, 7);
  EXPECT_TRUE(listed->front().capabilities.ledger_gateway);
  ASSERT_FALSE(listed->front().endpoints.empty());
  EXPECT_EQ(listed->front().endpoints.front().peer_id, harness->peer_id_b);
  EXPECT_EQ(listed->front().signing_public_key_b64, published.signing_public_key_b64);
  EXPECT_EQ(listed->front().kem_public_key_b64, published.kem_public_key_b64);
  EXPECT_EQ(listed->front().kem_binding_b64, published.kem_binding_b64);
  EXPECT_FALSE(listed->front().keys_update_needed);

  AmpDirectoryClient adapter(client);
  auto via_client = adapter.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(via_client)) << via_client.error().message;
  EXPECT_EQ(via_client->size(), 1u);
  EXPECT_FALSE(static_cast<bool>(adapter.SearchPeople("x")));

  client.Stop();
  seed.Stop();
}

TEST(AmpDirectoryProtocolTest, WrapLinkFailureMapsCodes) {
  using Mgr = pp::amp::PeerLinkManager;
  EXPECT_EQ(AmpDirectoryProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::DialTimeout, "slow")).GetCode(),
            AmpDirectoryProtocol::Err::Timeout);
  EXPECT_EQ(AmpDirectoryProtocol::WrapLinkFailure(Mgr::Failure::Of(Mgr::Err::ChannelOpenFailed, "mux")).GetCode(),
            AmpDirectoryProtocol::Err::ChannelFailed);
}

// The owner's chooser decides which query peers a list asks; none worth asking answers at once.
TEST(AmpDirectoryProtocolTest, AListAsksOnlyTheChosenQueryPeers) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("seed", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("client", harness->ma_a)));
  auto pump = [&]() { harness->PumpBoth(); };
  AmpDirectoryProtocol seed(*harness->runtime_b, pump);
  AmpDirectoryProtocol client(*harness->runtime_a, pump);
  AmpDirectoryProtocolConfig seed_cfg;
  seed_cfg.local_peer_id = harness->peer_id_b;
  seed.Configure(seed_cfg);
  MeshNodeHit published;
  published.account_id = "account:seed";
  published.entity_kind = "mesh_node";
  seed.SetNodesSnapshot({published});

  std::vector<std::string> offered;
  bool gone = false;
  AmpDirectoryProtocolConfig client_cfg;
  client_cfg.local_peer_id = harness->peer_id_a;
  client_cfg.query_peer_keys = {"gone-peer", "seed"};
  client_cfg.choose_query_peers = [&](const std::vector<std::string>& keys) {
    offered = keys;
    return gone ? std::vector<std::string>{} : std::vector<std::string>{"seed"};
  };
  client.Configure(client_cfg);
  seed.Start();
  client.Start();

  auto listed = client.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(listed)) << listed.error().message;
  EXPECT_EQ(offered, client_cfg.query_peer_keys);
  ASSERT_EQ(listed->size(), 1u);

  gone = true;
  auto none = client.ListMeshNodes();
  ASSERT_FALSE(static_cast<bool>(none));
  EXPECT_EQ(none.error().GetCode(), DirectoryClient::Err::NotFound);

  client.Stop();
  seed.Stop();
}

} // namespace
} // namespace pp::mesh
