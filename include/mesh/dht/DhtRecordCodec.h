#pragma once

#include "mesh/dht/DhtTypes.h"

#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace pp::mesh {

/**
 * `PeerRoutingRecord` v2 (pp-browser projects/release-compatibility/SIGNED_FORMATS § 2.10 and § 1, RC020; docs/DHT.md
 * § 4.5a). Its signed bytes:
 *
 *   "pp-mesh:dht-record" 00 ‖ u8 2
 *   ‖ Len("type=peer_routing") ‖ Len(peer_id) ‖ Len("seq=<decimal>") ‖ Len("ttl_seconds=<decimal>")
 *   ‖ Len("issued_at=<decimal>") ‖ u32 n ‖ n × Len(multiaddr) ‖ u8 caps_present [‖ u8 circuit_relay ‖ u8 media_relay]
 *   ‖ ext area                                     (u64 BE length ‖ entries: pp-cpp-common ExtArea)
 *
 * Len(x) is a u32 BE byte length, then the UTF-8 bytes: the fields from `type=peer_routing` on are v1's, unchanged; the
 * domain lost its `-v1`, the `sign_version=1` line became the version byte, and the extension area is new and last.
 */
inline constexpr std::string_view kPeerRoutingRecordDomain = "pp-mesh:dht-record";

std::vector<uint8_t> BuildPeerRoutingSignBytes(const PeerRoutingRecord& record);

/**
 * Why a record is refused, by the step of the receiver's order (SIGNED_FORMATS § 1.1) that refuses it: 1 version
 * (`Older`, `NewerVersion`), 2 parse (`Malformed`), 3 algorithms (`UnknownAlgorithm`), 4 signature (`BadSignature`),
 * 5 extensions (`UnknownCritical`). Only `BadSignature` (and `Malformed`) is the sender's fault; the rest are versions.
 */
enum class PeerRoutingRecordErr : int32_t {
  /** `version` missing or below 2 (a v1 record): made by an older build, outside the range. Not a forgery. */
  Older = 1,
  /** `version` above 2 (or not an integer): update needed — nothing parsed or verified. */
  NewerVersion = 2,
  /** Bad fields: type, peer_id, multiaddrs, `ext_b64`, a typed blob's id 0 or size. */
  Malformed = 3,
  /** The key or signature names an algorithm this build does not know: update needed — nothing verified. */
  UnknownAlgorithm = 4,
  /** The signature does not hold, or the key is not the record's subject's: a forgery. */
  BadSignature = 5,
  /** It verifies, and carries a critical extension entry this build does not understand: update needed. */
  UnknownCritical = 6,
};
using PeerRoutingRecordFailure = CodedFailure<PeerRoutingRecordErr>;
template <typename T>
using PeerRoutingRecordRoe = CodedRoe<T, PeerRoutingRecordErr>;

/** `NewerVersion`, `UnknownAlgorithm`, `UnknownCritical`: this build is behind, not the sender wrong. */
bool PeerRoutingRecordNeedsUpdate(PeerRoutingRecordErr err);
/** The refusal's name in the vector file: `older`, `newer_version`, `malformed`, `unknown_algorithm`, … */
std::string_view PeerRoutingRecordErrName(PeerRoutingRecordErr err);

/** The outside version (step 1 of the receiver's order), before any parse or signature check (V3). */
enum class PeerRoutingRecordSupport : int32_t {
  Supported = 0,
  /** Its `version` is newer than this build reads, or not a number: update needed. */
  UnsupportedVersion,
  /** Its `version` is below the floor (2), or missing: made by an older build. */
  OlderVersion,
};

/**
 * The record's `version`: 2 is supported. Neither other answer is misbehaviour: a record that is not supported is
 * skipped (counted as update needed, or older) and never held against the peer that served it; a STORE of one is
 * refused `bad_version`.
 */
PeerRoutingRecordSupport PeerRoutingRecordSupportOf(const common::Object& object);

/** v2 JSON: `version` 2, `ext_b64` only when the area is not empty, typed `signature_b64` and `public_key_b64`. */
common::Object PeerRoutingRecordToObject(const PeerRoutingRecord& record);

/**
 * Steps 1–3 of the receiver's order: the version (`Older`, `NewerVersion`), the fields (`Malformed`: also an `ext_b64`
 * that is not base64 of a valid area, a missing signature, a typed blob with id 0 or the wrong size), then the
 * algorithms of the signature and of the key when present (`UnknownAlgorithm`). Nothing is verified here.
 */
PeerRoutingRecordRoe<PeerRoutingRecord> PeerRoutingRecordFromObject(const common::Object& object);

/** `record` signed with a raw ML-DSA-65 secret key: `signature_b64` becomes the typed signature (id 1). */
Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record, std::span<const uint8_t> device_signing_secret);

/** A raw ML-DSA-65 public key as the record's `public_key_b64` (the typed blob, id 1, in base64). */
Roe<std::string> PeerRoutingPublicKeyB64(std::span<const uint8_t> ml_dsa_65_public_key);

/**
 * Step 4 with a key the caller already trusts for the subject (its authenticated session key, raw ML-DSA-65): the
 * typed signature must be of that algorithm (`BadSignature` otherwise) and hold over the rebuilt signed bytes. The
 * extensions are not looked at (`PeerRoutingRecordHasUnknownCritical`).
 */
PeerRoutingRecordRoe<void> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                                   std::span<const uint8_t> signing_public_key);

/**
 * Step 4 for a record checked on its own: it carries a typed public key (`Malformed` if none), of the signature's
 * algorithm, whose raw key derives its PeerId, and its signature holds (`BadSignature` otherwise). How a record
 * answered by another node is accepted.
 */
PeerRoutingRecordRoe<void> VerifyPeerRoutingRecordStandalone(const PeerRoutingRecord& record);

/**
 * Step 5, after the signature holds: a critical entry this build does not understand (none is defined at release
 * 1). Such a record is "update needed": not stored, not used.
 */
bool PeerRoutingRecordHasUnknownCritical(const PeerRoutingRecord& record);

/**
 * The whole receiver's order for a record checked on its own (steps 1–5): `PeerRoutingRecordFromObject`, then
 * `VerifyPeerRoutingRecordStandalone`, then `UnknownCritical`. Semantics (expiry, seq) are the caller's.
 */
PeerRoutingRecordRoe<PeerRoutingRecord> ReadPeerRoutingRecord(const common::Object& object);

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, int64_t now_seconds, int64_t grace_seconds = 60);

/** Where a DHT contact is dialled: the record's first ADP multiaddr, else its first; empty if none. */
std::string PeerRoutingRecordDialAddr(const PeerRoutingRecord& record);

common::Value DhtCloserPeersToArray(const std::vector<DhtCloserPeer>& peers);
/**
 * A response's `closer_peers`: at most `max`, each a base58 PeerId with a multiaddr that ends in
 * that PeerId, no duplicates; malformed entries are skipped.
 */
std::vector<DhtCloserPeer> DhtCloserPeersFromResponse(const common::Object& response, size_t max = kDhtCloserPeers);

/** Map DHT record → directory-shaped node for hop policy (n2-caps). */
MeshDirectoryNode MeshDirectoryNodeFromDhtRecord(const PeerRoutingRecord& record);
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromDhtRecords(const std::vector<PeerRoutingRecord>& records);

} // namespace pp::mesh
