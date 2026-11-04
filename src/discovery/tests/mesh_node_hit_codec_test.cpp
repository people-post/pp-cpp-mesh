#include "mesh/discovery/MeshNodeHitCodec.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

TEST(MeshNodeHitCodecTest, KeysAndTheirBindingRoundTrip) {
  MeshNodeHit hit;
  hit.relay_user_id = "relay-user-1";
  hit.account_id = "account:abc";
  hit.signing_public_key_b64 = "c2lnbmluZw==";
  hit.kem_public_key_b64 = "a2Vt";
  hit.kem_binding_signature_b64 = "YmluZGluZw==";

  auto decoded = MeshNodeHitFromObject(MeshNodeHitToObject(hit));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->signing_public_key_b64, hit.signing_public_key_b64);
  EXPECT_EQ(decoded->kem_public_key_b64, hit.kem_public_key_b64);
  EXPECT_EQ(decoded->kem_binding_signature_b64, hit.kem_binding_signature_b64);
}

TEST(MeshNodeHitCodecTest, AHitWithoutABindingDecodesWithoutOne) {
  MeshNodeHit hit;
  hit.relay_user_id = "relay-user-1";
  hit.kem_public_key_b64 = "a2Vt";

  auto decoded = MeshNodeHitFromObject(MeshNodeHitToObject(hit));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_FALSE(decoded->kem_binding_signature_b64.has_value());
}

// N032: what a node says it runs travels with its row; a row without it (before N032) decodes as "not said".
TEST(MeshNodeHitCodecTest, WhatANodeSaysItRunsRoundTripsAndIsOptional) {
  MeshNodeHit hit;
  hit.relay_user_id = "relay-user-1";
  hit.amp_wire = 2;
  hit.node_version = "4.1.0";
  hit.features = {"hop_data_lane"};
  auto decoded = MeshNodeHitFromObject(MeshNodeHitToObject(hit));
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->amp_wire, 2);
  EXPECT_EQ(decoded->node_version, "4.1.0");
  EXPECT_EQ(decoded->features, hit.features);

  MeshNodeHit before;
  before.relay_user_id = "relay-user-2";
  const auto object = MeshNodeHitToObject(before);
  EXPECT_FALSE(object.contains("amp_wire")) << "nothing said, nothing sent";
  auto old = MeshNodeHitFromObject(object);
  ASSERT_TRUE(old) << old.error().message;
  EXPECT_EQ(old->amp_wire, 0);
  EXPECT_TRUE(old->node_version.empty());
  EXPECT_TRUE(old->features.empty());
}

} // namespace
} // namespace pp::mesh
