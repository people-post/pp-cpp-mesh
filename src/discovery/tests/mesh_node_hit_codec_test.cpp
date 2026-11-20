#include "mesh/discovery/MeshNodeHitCodec.h"

#include "common/ExtArea.h"
#include "crypto/AlgId.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

/** A typed blob (id ‖ body of `size` bytes) in base64. */
std::string Typed(const uint8_t id, const size_t size, const uint8_t fill = 0x5a) {
  pp::ByteVector bytes(1 + size, fill);
  bytes[0] = id;
  return pp::Base64Encode(bytes);
}

/** A KEM binding blob: version ‖ ext area ‖ u64 BE length ‖ typed signature (`tail` bytes appended after). */
std::string Binding(const uint8_t version, const uint8_t sig_id = 1, const pp::ExtArea& ext = {}, const size_t tail = 0,
                    const size_t sig_size = pp::kMlDsa65SignatureBytes) {
  pp::ByteVector bytes{version};
  ext.AppendTo(bytes);
  const uint64_t length = 1 + sig_size;
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<uint8_t>((length >> shift) & 0xff));
  }
  bytes.push_back(sig_id);
  bytes.insert(bytes.end(), sig_size + tail, 0x33);
  return pp::Base64Encode(bytes);
}

MeshNodeHit HitWithKeys() {
  MeshNodeHit hit;
  hit.account_id = "account:abc";
  hit.signing_public_key_b64 = Typed(pp::kSignatureAlgMlDsa65, pp::kMlDsa65PublicKeyBytes);
  hit.kem_public_key_b64 = Typed(pp::kKemAlgMlKem768, 1184);
  hit.kem_binding_b64 = Binding(kKemBindingVersion);
  DirectoryEndpoint ep;
  ep.peer_id = "QmNode";
  ep.multiaddrs = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/QmNode"};
  hit.endpoints.push_back(ep);
  return hit;
}

TEST(MeshNodeHitCodecTest, KeysAndTheirBindingRoundTrip) {
  const MeshNodeHit hit = HitWithKeys();
  const common::Object object = MeshNodeHitToObject(hit);
  EXPECT_TRUE(object.contains("kem_binding_b64"));
  EXPECT_FALSE(object.contains("kem_binding_signature_b64")) << "v1's field is gone";
  EXPECT_FALSE(object.contains("signature_alg")) << "the typed blobs name their algorithms";

  auto decoded = MeshNodeHitFromObject(object);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->signing_public_key_b64, hit.signing_public_key_b64);
  EXPECT_EQ(decoded->kem_public_key_b64, hit.kem_public_key_b64);
  EXPECT_EQ(decoded->kem_binding_b64, hit.kem_binding_b64);
  EXPECT_FALSE(decoded->keys_update_needed);
}

TEST(MeshNodeHitCodecTest, AHitWithoutABindingDecodesWithoutOne) {
  MeshNodeHit hit;
  hit.account_id = "account:node-1";
  hit.kem_public_key_b64 = Typed(pp::kKemAlgMlKem768, 1184);

  auto decoded = MeshNodeHitFromObject(MeshNodeHitToObject(hit));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_FALSE(decoded->kem_binding_b64.has_value());
  EXPECT_TRUE(decoded->kem_public_key_b64.has_value());
}

TEST(MeshNodeHitCodecTest, KeyShapesFollowTheReceiversOrder) {
  using S = DirectoryKeyShape;
  EXPECT_EQ(SigningPublicKeyShape(Typed(1, pp::kMlDsa65PublicKeyBytes)), S::Ok);
  EXPECT_EQ(SigningPublicKeyShape(Typed(2, 10)), S::UpdateNeeded) << "an unknown id: its size is unknown too";
  EXPECT_EQ(SigningPublicKeyShape(Typed(0, pp::kMlDsa65PublicKeyBytes)), S::Malformed) << "id 0 is invalid";
  EXPECT_EQ(SigningPublicKeyShape(Typed(1, 100)), S::Malformed) << "a known id with the wrong size";
  EXPECT_EQ(SigningPublicKeyShape(pp::Base64Encode(pp::ByteVector(pp::kMlDsa65PublicKeyBytes, 1))), S::Malformed)
      << "v1's raw key";
  EXPECT_EQ(SigningPublicKeyShape(""), S::Malformed);
  EXPECT_EQ(SigningPublicKeyShape("***"), S::Malformed);
  EXPECT_EQ(KemPublicKeyShape(Typed(1, 1184)), S::Ok);
  EXPECT_EQ(KemPublicKeyShape(Typed(1, pp::kMlDsa65PublicKeyBytes)), S::Malformed) << "a signing key is not a KEM key";
  EXPECT_EQ(KemPublicKeyShape(Typed(9, 4)), S::UpdateNeeded);

  EXPECT_EQ(KemBindingShape(Binding(2)), S::Ok);
  auto area = pp::ExtArea::FromEntries({{0x0001, {1, 2, 3}}, {0x8001, {}}});
  ASSERT_TRUE(static_cast<bool>(area));
  EXPECT_EQ(KemBindingShape(Binding(2, 1, *area)), S::Ok)
      << "entries are kept: a critical one is the app's to judge, after it verifies";
  EXPECT_EQ(KemBindingShape(Binding(3, 7, {}, 5, 2)), S::UpdateNeeded) << "a newer binding: nothing after it is read";
  EXPECT_EQ(KemBindingShape(Binding(1)), S::Older);
  EXPECT_EQ(KemBindingShape(pp::Base64Encode(pp::ByteVector(pp::kMlDsa65SignatureBytes, 0x01))), S::Older)
      << "v1's bare signature";
  EXPECT_EQ(KemBindingShape(Binding(2, 2, {}, 0, 50)), S::UpdateNeeded) << "a signature of an unknown algorithm";
  EXPECT_EQ(KemBindingShape(Binding(2, 0)), S::Malformed);
  EXPECT_EQ(KemBindingShape(Binding(2, 1, {}, 0, 100)), S::Malformed) << "a wrong signature size";
  EXPECT_EQ(KemBindingShape(Binding(2, 1, {}, 1)), S::Malformed) << "a byte left over";
  EXPECT_EQ(KemBindingShape(pp::Base64Encode(pp::ByteVector{2, 0, 0, 0, 0, 0, 0, 0, 3, 0x80})), S::Malformed)
      << "an area past the end";
  EXPECT_EQ(KemBindingShape(pp::Base64Encode(pp::ByteVector{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})), S::Malformed)
      << "no room for the signature's length";
}

// A key the mesh cannot read for its shape is dropped; one of an unknown algorithm is kept and flagged. The row stays
// either way: its endpoints and capabilities do not depend on the keys.
TEST(MeshNodeHitCodecTest, KeysAreKeptFlaggedOrDroppedAndTheRowStays) {
  common::Object newer = MeshNodeHitToObject(HitWithKeys());
  newer.set("signing_public_key_b64", Typed(2, 10));
  newer.set("signature_alg", "ml-dsa-87"); // a v1 row's field: ignored
  auto flagged = MeshNodeHitFromObject(newer);
  ASSERT_TRUE(flagged) << flagged.error().message;
  EXPECT_TRUE(flagged->keys_update_needed);
  EXPECT_EQ(flagged->signing_public_key_b64, newer.getString("signing_public_key_b64")) << "kept as received";
  EXPECT_EQ(MeshNodeHitToObject(*flagged).getString("signing_public_key_b64"),
            newer.getString("signing_public_key_b64"));

  common::Object newer_binding = MeshNodeHitToObject(HitWithKeys());
  newer_binding.set("kem_binding_b64", Binding(3));
  auto flagged_binding = MeshNodeHitFromObject(newer_binding);
  ASSERT_TRUE(flagged_binding);
  EXPECT_TRUE(flagged_binding->keys_update_needed);
  EXPECT_TRUE(flagged_binding->kem_binding_b64.has_value());

  common::Object v1 = MeshNodeHitToObject(HitWithKeys()); // what a v9 node served
  v1.erase("kem_binding_b64");
  v1.set("kem_binding_signature_b64", pp::Base64Encode(pp::ByteVector(pp::kMlDsa65SignatureBytes, 1)));
  v1.set("signing_public_key_b64", pp::Base64Encode(pp::ByteVector(pp::kMlDsa65PublicKeyBytes, 1)));
  auto old = MeshNodeHitFromObject(v1);
  ASSERT_TRUE(old) << "the row stays";
  EXPECT_FALSE(old->signing_public_key_b64.has_value()) << "an untyped key is malformed: dropped";
  EXPECT_FALSE(old->kem_binding_b64.has_value()) << "v1's field is not read";
  EXPECT_TRUE(old->kem_public_key_b64.has_value());
  EXPECT_FALSE(old->keys_update_needed);
  ASSERT_EQ(old->endpoints.size(), 1u);

  common::Object older_binding = MeshNodeHitToObject(HitWithKeys());
  older_binding.set("kem_binding_b64", Binding(1));
  auto dropped = MeshNodeHitFromObject(older_binding);
  ASSERT_TRUE(dropped);
  EXPECT_FALSE(dropped->kem_binding_b64.has_value());
  EXPECT_FALSE(dropped->keys_update_needed);
}

// N032: what a node says it runs travels with its row; a row without it (before N032) decodes as "not said".
TEST(MeshNodeHitCodecTest, WhatANodeSaysItRunsRoundTripsAndIsOptional) {
  MeshNodeHit hit;
  hit.account_id = "account:node-1";
  hit.amp_wire = 2;
  hit.node_version = "4.1.0";
  hit.features = {"hop_data_lane"};
  auto decoded = MeshNodeHitFromObject(MeshNodeHitToObject(hit));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->amp_wire, 2);
  EXPECT_EQ(decoded->node_version, "4.1.0");
  EXPECT_EQ(decoded->features, hit.features);

  MeshNodeHit before;
  before.account_id = "account:node-2";
  const auto object = MeshNodeHitToObject(before);
  EXPECT_FALSE(object.contains("amp_wire")) << "nothing said, nothing sent";
  auto old = MeshNodeHitFromObject(object);
  ASSERT_TRUE(old) << old.error().message;
  EXPECT_EQ(old->amp_wire, 0);
  EXPECT_TRUE(old->node_version.empty());
  EXPECT_TRUE(old->features.empty());
}

// The Account ID is the row's key (pp-browser multi-relay MR001): a row without one is refused.
TEST(MeshNodeHitCodecTest, ARowWithoutAnAccountIdIsRefused) {
  MeshNodeHit hit;
  hit.entity_kind = "mesh_node";
  common::Object object = MeshNodeHitToObject(hit);
  EXPECT_FALSE(MeshNodeHitFromObject(object));
  object.set("relay_user_id", "relay:old");
  EXPECT_FALSE(MeshNodeHitFromObject(object)) << "a relay id is not an Account ID";
}

} // namespace
} // namespace pp::mesh
