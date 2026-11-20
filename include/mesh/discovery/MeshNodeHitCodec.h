#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <string>
namespace pp::mesh {

/**
 * A directory row (`nodes` on /pp-mesh/directory/1.0.0; docs/DIRECTORY.md). Its key fields are read for their shape
 * only (`*Shape` below): a malformed or older one is dropped (absent: the app sees a row without usable keys), one of
 * an algorithm or binding version this build does not know is kept as received and flags `keys_update_needed`. The
 * rest of the row (endpoints, capabilities) is kept either way. Unknown keys, `signature_alg` among them, are ignored.
 */
common::Object MeshNodeHitToObject(const MeshNodeHit& hit);
Roe<MeshNodeHit> MeshNodeHitFromObject(const common::Object& object);
common::Value MeshNodeHitsToJsonArray(const std::vector<MeshNodeHit>& hits);
Roe<std::vector<MeshNodeHit>> MeshNodeHitsFromJsonArray(const common::Array& nodes);

/** The KEM binding blob's version (pp-browser SIGNED_FORMATS § 2.3): its first byte. */
inline constexpr uint8_t kKemBindingVersion = 2;

/**
 * The shape of a row's key field, in the receiver's order (SIGNED_FORMATS § 1.1) as far as a carrier can go: version,
 * parse, algorithms. Nothing is verified (the account's signature is the app's to check), so an unknown critical
 * extension in the binding (step 5, after the signature) is not judged here: the bytes are kept for the app.
 */
enum class DirectoryKeyShape : int32_t {
  Ok = 0,
  /** An algorithm id (or a binding version) above what this build knows: kept, update needed. */
  UpdateNeeded,
  /** A binding version below 2 (v1's bare signature): dropped. */
  Older,
  /** Not base64, empty, id 0, a wrong size; a binding area or length that does not parse: dropped. */
  Malformed,
};

/** `signing_public_key_b64`: a typed signature public key (ml-dsa-65: id 1, then 1952 bytes). */
DirectoryKeyShape SigningPublicKeyShape(const std::string& b64);
/** `kem_public_key_b64`: a typed KEM public key (ml-kem-768: id 1, then 1184 bytes). */
DirectoryKeyShape KemPublicKeyShape(const std::string& b64);
/** `kem_binding_b64`: `u8 2 ‖ u64 BE len ‖ ext entries ‖ u64 BE len ‖ typed signature`, nothing after it. */
DirectoryKeyShape KemBindingShape(const std::string& b64);

} // namespace pp::mesh
