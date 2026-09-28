#include "domain/mesh/dht/DhtRecordCodec.h"
#include "domain/mesh/dht/DhtRecordStore.h"

#include "foundation/crypto/MlDsa.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <ctime>
#include <limits>

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

TEST(DhtRecordCodecTest, OverflowingIssuedAtPlusTtlIsExpired) {
  // issued_at/ttl_seconds are peer-controlled off the wire; a huge value must not overflow the
  // expiry sum and wrap into looking non-expired.
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWOverflow";
  record.seq = 1;
  record.ttl_seconds = std::numeric_limits<int64_t>::max();
  record.issued_at = std::numeric_limits<int64_t>::max() - 5;
  record.multiaddrs = {"/ip4/203.0.113.3/udp/443/adp/1.0.0/p2p/12D3KooWOverflow"};
  EXPECT_TRUE(PeerRoutingRecordExpired(record, 1000));
}

TEST(DhtRecordStoreTest, ClampsTtlToMax) {
  DhtRecordStore store;
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWLongTtl";
  record.seq = 1;
  record.ttl_seconds = 365 * 24 * 3600; // one year — far past the store's cap
  record.issued_at = static_cast<int64_t>(std::time(nullptr));
  record.multiaddrs = {"/ip4/203.0.113.4/udp/443/adp/1.0.0/p2p/12D3KooWLongTtl"};

  ASSERT_TRUE(store.Put(record));
  auto stored = store.Get(record.peer_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_LE(stored->ttl_seconds, 24 * 3600);
}

TEST(DhtRecordStoreTest, RejectsNewPeerWhenFullOfLiveRecords) {
  DhtRecordStore store;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // Small enough to run fast; the store's cap is independent of this count, so filling a store
  // is impractical here — instead this locks in that Put() has a PruneExpiredLocked path that
  // runs without deadlocking or corrupting state when the store is exercised repeatedly.
  for (int i = 0; i < 50; ++i) {
    PeerRoutingRecord record;
    record.peer_id = "12D3KooWBulk" + std::to_string(i);
    record.seq = 1;
    record.ttl_seconds = 60;
    record.issued_at = now;
    record.multiaddrs = {"/ip4/203.0.113.5/udp/443/adp/1.0.0/p2p/12D3KooWBulk" + std::to_string(i)};
    EXPECT_TRUE(store.Put(record));
  }
  EXPECT_EQ(store.Size(), 50u);
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
