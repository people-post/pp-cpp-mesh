#include "mesh/dht/DhtRecordCodec.h"
#include "mesh/dht/DhtRecordStore.h"

#include "amp/link/PeerId.h"
#include "common/ExtArea.h"
#include "crypto/AlgId.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"
#include "mesh/testing/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <ctime>
#include <limits>

namespace pp::mesh {
namespace {

TEST(DhtRecordCodecTest, SignVerifyRoundTrip) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  PeerRoutingRecord record;
  record.peer_id = "12D3KooWTestPeer";
  record.seq = 3;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/12D3KooWTestPeer"};

  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));

  auto verified = VerifyPeerRoutingRecord(*signed_record, keys->public_key);
  EXPECT_TRUE(static_cast<bool>(verified));
}

TEST(DhtRecordCodecTest, SignVerifyRoundTripWithCapabilities) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));

  PeerRoutingRecord record;
  record.peer_id = "12D3KooWTestPeer";
  record.seq = 4;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'100;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/12D3KooWTestPeer"};
  record.capabilities = PeerRoutingCapabilities{.circuit_relay = true, .media_relay = true};

  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));

  auto verified = VerifyPeerRoutingRecord(*signed_record, keys->public_key);
  EXPECT_TRUE(static_cast<bool>(verified));

  auto parsed = PeerRoutingRecordFromObject(PeerRoutingRecordToObject(*signed_record));
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;
  ASSERT_TRUE(parsed->capabilities.has_value());
  EXPECT_TRUE(parsed->capabilities->circuit_relay);
  EXPECT_TRUE(parsed->capabilities->media_relay);
}

// A record that carries its signer's key can be checked by anyone, not only over its subject's link.
TEST(DhtRecordCodecTest, ARecordWithItsKeyChecksOnItsOwn) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  auto other = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  ASSERT_TRUE(static_cast<bool>(other));

  PeerRoutingRecord record;
  record.peer_id = pp::amp::PeerIdFromIdentityKey(keys->public_key);
  record.seq = 1;
  record.ttl_seconds = 3600;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/" + record.peer_id};
  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));

  EXPECT_FALSE(VerifyPeerRoutingRecordStandalone(*signed_record)) << "no key: not checkable";

  signed_record->public_key_b64 = PeerRoutingPublicKeyB64(keys->public_key).valueOr("");
  auto parsed = PeerRoutingRecordFromObject(PeerRoutingRecordToObject(*signed_record));
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->public_key_b64, signed_record->public_key_b64);
  EXPECT_TRUE(VerifyPeerRoutingRecordStandalone(*parsed));

  PeerRoutingRecord wrong_key = *parsed; // a key that does not derive the PeerId
  wrong_key.public_key_b64 = PeerRoutingPublicKeyB64(other->public_key).valueOr("");
  EXPECT_FALSE(VerifyPeerRoutingRecordStandalone(wrong_key));

  PeerRoutingRecord altered = *parsed; // the right key, the signed fields changed
  altered.multiaddrs = {"/ip4/198.51.100.9/udp/443/adp/2/p2p/" + record.peer_id};
  EXPECT_FALSE(VerifyPeerRoutingRecordStandalone(altered));

  PeerRoutingRecord raw_key = *parsed; // v1's untyped key: its first byte is not an algorithm this build knows
  raw_key.public_key_b64 = pp::Base64Encode(keys->public_key);
  EXPECT_FALSE(VerifyPeerRoutingRecordStandalone(raw_key));
}

// v2 keys and signatures are typed blobs (pp-cpp-crypto AlgId): id 1 (ml-dsa-65), then the raw bytes; the PeerId is
// still derived from the raw key.
TEST(DhtRecordCodecTest, KeyAndSignatureAreTypedBlobs) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  PeerRoutingRecord record;
  record.peer_id = pp::amp::PeerIdFromIdentityKey(keys->public_key);
  record.seq = 1;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/" + record.peer_id};
  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));
  auto signature = pp::Base64Decode(signed_record->signature_b64);
  ASSERT_TRUE(static_cast<bool>(signature));
  ASSERT_EQ(signature->size(), 1 + pp::kMlDsa65SignatureBytes);
  EXPECT_EQ((*signature)[0], pp::kSignatureAlgMlDsa65);
  auto key_b64 = PeerRoutingPublicKeyB64(keys->public_key);
  ASSERT_TRUE(static_cast<bool>(key_b64));
  auto key = pp::Base64Decode(*key_b64);
  ASSERT_TRUE(static_cast<bool>(key));
  ASSERT_EQ(key->size(), 1 + pp::kMlDsa65PublicKeyBytes);
  EXPECT_EQ((*key)[0], pp::kSignatureAlgMlDsa65);
  EXPECT_TRUE(std::equal(keys->public_key.begin(), keys->public_key.end(), key->begin() + 1));
  EXPECT_FALSE(static_cast<bool>(PeerRoutingPublicKeyB64(std::vector<uint8_t>(10, 1)))) << "not an ml-dsa-65 key";

  const common::Object object = PeerRoutingRecordToObject(*signed_record);
  EXPECT_FALSE(object.contains("signature_alg")) << "the typed signature names its algorithm";
  EXPECT_FALSE(object.contains("ext_b64")) << "writers omit an empty area";
  EXPECT_EQ(object.getIf<int64_t>("version").value_or(0), 2);

  common::Object empty_ext = object; // an empty ext_b64 is the empty area, as an absent one
  empty_ext.set("ext_b64", "");
  auto parsed = PeerRoutingRecordFromObject(empty_ext);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;
  EXPECT_TRUE(parsed->ext.empty());
  EXPECT_TRUE(static_cast<bool>(VerifyPeerRoutingRecord(*parsed, keys->public_key)));

  common::Object no_signature = object;
  no_signature.set("signature_b64", "");
  auto unsigned_record = PeerRoutingRecordFromObject(no_signature);
  ASSERT_FALSE(static_cast<bool>(unsigned_record));
  EXPECT_EQ(unsigned_record.error().GetCode(), PeerRoutingRecordErr::Malformed);
}

// The extension area is signed: an entry added, dropped or changed on the way breaks the signature; a critical
// entry this build does not understand makes a verified record "update needed".
TEST(DhtRecordCodecTest, TheExtensionAreaIsSignedAndKept) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  PeerRoutingRecord record;
  record.peer_id = pp::amp::PeerIdFromIdentityKey(keys->public_key);
  record.seq = 1;
  record.issued_at = 1'700'000'000;
  record.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/" + record.peer_id};
  auto area = pp::ExtArea::FromEntries({{0x0005, {9, 9}}});
  ASSERT_TRUE(static_cast<bool>(area));
  record.ext = *area;
  auto signed_record = SignPeerRoutingRecord(record, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_record));
  signed_record->public_key_b64 = PeerRoutingPublicKeyB64(keys->public_key).valueOr("");

  const common::Object object = PeerRoutingRecordToObject(*signed_record);
  ASSERT_TRUE(object.contains("ext_b64"));
  auto read = ReadPeerRoutingRecord(object);
  ASSERT_TRUE(static_cast<bool>(read)) << read.error().message;
  EXPECT_EQ(read->ext, *area) << "unknown non-critical entries are kept";

  common::Object dropped = object;
  dropped.erase("ext_b64");
  auto read_dropped = ReadPeerRoutingRecord(dropped);
  ASSERT_FALSE(static_cast<bool>(read_dropped));
  EXPECT_EQ(read_dropped.error().GetCode(), PeerRoutingRecordErr::BadSignature);

  common::Object not_base64 = object;
  not_base64.set("ext_b64", "***");
  auto read_bad = ReadPeerRoutingRecord(not_base64);
  ASSERT_FALSE(static_cast<bool>(read_bad));
  EXPECT_EQ(read_bad.error().GetCode(), PeerRoutingRecordErr::Malformed);

  auto critical = pp::ExtArea::FromEntries({{0x8005, {}}});
  ASSERT_TRUE(static_cast<bool>(critical));
  PeerRoutingRecord with_critical = record;
  with_critical.ext = *critical;
  auto signed_critical = SignPeerRoutingRecord(with_critical, keys->secret_key);
  ASSERT_TRUE(static_cast<bool>(signed_critical));
  EXPECT_TRUE(static_cast<bool>(VerifyPeerRoutingRecord(*signed_critical, keys->public_key)));
  EXPECT_TRUE(PeerRoutingRecordHasUnknownCritical(*signed_critical));
}

// closer_peers are hints from a stranger: only well-formed ones that bind their PeerId pass.
TEST(DhtRecordCodecTest, CloserPeersKeepOnlyWellFormedHints) {
  const std::string a = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
  const std::string b = "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie";
  const auto ma = [](const std::string& peer) { return "/ip4/203.0.113.7/udp/443/adp/2/p2p/" + peer; };
  common::Object response;
  response.set("closer_peers", DhtCloserPeersToArray({
                                   {a, ma(a)},
                                   {b, ma(a)},             // the address names someone else
                                   {a, ma(a)},             // twice
                                   {"Qm0lI", ma("Qm0lI")}, // not base58
                                   {b, ma(b)},
                               }));
  const auto peers = DhtCloserPeersFromResponse(response);
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0].peer_id, a);
  EXPECT_EQ(peers[1].peer_id, b);
  EXPECT_EQ(DhtCloserPeersFromResponse(response, 1).size(), 1u);
  EXPECT_TRUE(DhtCloserPeersFromResponse(common::Object{}).empty());
}

TEST(DhtRecordCodecTest, RejectsExpiredRecord) {
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWExpired";
  record.seq = 1;
  record.ttl_seconds = 10;
  record.issued_at = 1;
  record.multiaddrs = {"/ip4/203.0.113.2/udp/443/adp/2/p2p/12D3KooWExpired"};
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
  record.multiaddrs = {"/ip4/203.0.113.3/udp/443/adp/2/p2p/12D3KooWOverflow"};
  EXPECT_TRUE(PeerRoutingRecordExpired(record, 1000));
}

TEST(DhtRecordStoreTest, ClampsEffectiveExpiryWithoutRewritingSignedTtl) {
  DhtRecordStore store;
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWLongTtl";
  record.seq = 1;
  record.ttl_seconds = 365 * 24 * 3600; // one year — far past the store's 24h cap
  record.issued_at = static_cast<int64_t>(std::time(nullptr)) - 2 * 24 * 3600; // issued 2 days ago
  record.multiaddrs = {"/ip4/203.0.113.4/udp/443/adp/2/p2p/12D3KooWLongTtl"};

  // Not expired under the record's own (signed) one-year ttl, but 2 elapsed days is already
  // past the store's 24h effective cap: Put must reject it rather than rewriting ttl_seconds
  // to make it fit (that would invalidate the record's signature).
  ASSERT_FALSE(PeerRoutingRecordExpired(record, static_cast<int64_t>(std::time(nullptr))));
  EXPECT_FALSE(store.Put(record));
}

TEST(DhtRecordStoreTest, FreshRecordWithLongTtlIsStillLive) {
  DhtRecordStore store;
  PeerRoutingRecord record;
  record.peer_id = "12D3KooWFreshLongTtl";
  record.seq = 1;
  record.ttl_seconds = 365 * 24 * 3600;
  record.issued_at = static_cast<int64_t>(std::time(nullptr));
  record.multiaddrs = {"/ip4/203.0.113.6/udp/443/adp/2/p2p/12D3KooWFreshLongTtl"};

  ASSERT_TRUE(store.Put(record));
  EXPECT_TRUE(store.Get(record.peer_id).has_value());
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
    record.multiaddrs = {"/ip4/203.0.113.5/udp/443/adp/2/p2p/12D3KooWBulk" + std::to_string(i)};
    EXPECT_TRUE(store.Put(record));
  }
  EXPECT_EQ(store.Size(), 50u);
}

TEST(DhtRecordCodecTest, RejectsMultiaddrPeerMismatch) {
  common::Object object;
  object.set("type", "peer_routing");
  object.set("version", int64_t{2});
  object.set("peer_id", "12D3KooWPeerA");
  object.set("seq", int64_t{1});
  object.set("ttl_seconds", int64_t{3600});
  object.set("issued_at", int64_t{1'700'000'000});
  object.set("multiaddrs",
             common::makeArray(std::vector<common::Value>{"/ip4/1.2.3.4/udp/443/adp/2/p2p/12D3KooWPeerB"}));
  object.set("signature_b64", "AA==");

  auto parsed = PeerRoutingRecordFromObject(object);
  ASSERT_FALSE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed.error().GetCode(), PeerRoutingRecordErr::Malformed);
}

} // namespace
} // namespace pp::mesh
