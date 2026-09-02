#include "domain/mesh/dht/DhtRecordCodec.h"

#include "foundation/crypto/MlDsa.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(DhtRecordCodecTest, SignVerifyRoundTrip) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  PeerRoutingRecord record;
  record.peer_id = "12D3KooWTestPeer";
  record.seq = 3;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/1.0.0/p2p/12D3KooWTestPeer"};

  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));

  auto verified = VerifyPeerRoutingRecord(*signed_record, keys->public_key);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);
}

TEST(DhtRecordCodecTest, SignVerifyRoundTripWithCapabilities) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  PeerRoutingRecord record;
  record.peer_id = "12D3KooWTestPeer";
  record.seq = 4;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'100;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/1.0.0/p2p/12D3KooWTestPeer"};
  record.capabilities = PeerRoutingCapabilities{.circuit_relay = true, .media_relay = true};

  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));

  auto verified = VerifyPeerRoutingRecord(*signed_record, keys->public_key);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);

  auto parsed = PeerRoutingRecordFromObject(PeerRoutingRecordToObject(*signed_record));
  ASSERT_TRUE(static_cast<bool>(parsed));
  ASSERT_TRUE(parsed->capabilities.has_value());
  EXPECT_TRUE(parsed->capabilities->circuit_relay);
  EXPECT_TRUE(parsed->capabilities->media_relay);
}

TEST(DhtRecordCodecTest, RejectsExpiredRecord) {
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWExpired";
  record.seq = 1;
  record.ttl_seconds = 10;
  record.issued_at = 1;
  record.multiaddrs = {"/ip4/203.0.113.2/udp/443/adp/1.0.0/p2p/12D3KooWExpired"};
  EXPECT_TRUE(PeerRoutingRecordExpired(record, 1000));
}

TEST(DhtRecordCodecTest, RejectsMultiaddrPeerMismatch) {
  Object object;
  object.set("type", "peer_routing");
  object.set("peer_id", "12D3KooWPeerA");
  object.set("seq", int64_t{1});
  object.set("ttl_seconds", int64_t{3600});
  object.set("issued_at", int64_t{1'700'000'000});
  object.set("multiaddrs", makeArray(std::vector<Value>{"/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/12D3KooWPeerB"}));
  object.set("signature_b64", "AA==");
  object.set("signature_alg", "ml-dsa-65");

  auto parsed = PeerRoutingRecordFromObject(object);
  EXPECT_FALSE(static_cast<bool>(parsed));
}

} // namespace
} // namespace pbr
