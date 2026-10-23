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

} // namespace
} // namespace pp::mesh
