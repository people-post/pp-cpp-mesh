#include "mesh/dht/DhtRecordCodec.h"

#include "mesh/dht/DhtKey.h"

#include "amp/link/PeerId.h"
#include "common/Utilities.h"
#include "crypto/AlgId.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace pp::mesh {
namespace {

void AppendU32Be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void AppendLenPrefixedUtf8(std::vector<uint8_t>& out, std::string_view text) {
  AppendU32Be(out, static_cast<uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

/** Base64 of possibly nothing: pp::Base64Decode must not be given an empty string (libsodium gets a null pointer). */
Roe<pp::ByteVector> DecodeB64(const std::string& b64) {
  if (b64.empty()) {
    return pp::ByteVector{};
  }
  return pp::Base64Decode(b64);
}

PeerRoutingRecordFailure Fail(const PeerRoutingRecordErr err, std::string message) {
  return PeerRoutingRecordFailure::Of(err, std::move(message));
}

/** A typed blob in base64 (steps 2–3): not base64, id 0 or a wrong size → Malformed; an unknown id → UnknownAlgorithm. */
PeerRoutingRecordRoe<pp::TypedBlob> DecodeTypedB64(const pp::BlobKind kind, const std::string& b64,
                                                   const std::string_view field) {
  auto bytes = DecodeB64(b64);
  if (!bytes) {
    return Fail(PeerRoutingRecordErr::Malformed, std::string(field) + " is not base64");
  }
  auto blob = pp::DecodeTypedBlob(kind, *bytes);
  if (!blob) {
    if (pp::IsUnknownAlgorithm(blob.error())) {
      return Fail(PeerRoutingRecordErr::UnknownAlgorithm,
                  std::string(field) + ": an algorithm this build does not know (update needed)");
    }
    return Fail(PeerRoutingRecordErr::Malformed, std::string(field) + ": " + blob.error().message);
  }
  return std::move(*blob);
}

} // namespace

std::vector<uint8_t> BuildPeerRoutingSignBytes(const PeerRoutingRecord& record) {
  std::vector<uint8_t> out;
  out.insert(out.end(), kPeerRoutingRecordDomain.begin(), kPeerRoutingRecordDomain.end());
  out.push_back(0x00);
  out.push_back(static_cast<uint8_t>(kPeerRoutingRecordVersion));
  AppendLenPrefixedUtf8(out, "type=peer_routing");
  AppendLenPrefixedUtf8(out, record.peer_id);
  AppendLenPrefixedUtf8(out, "seq=" + std::to_string(record.seq));
  AppendLenPrefixedUtf8(out, "ttl_seconds=" + std::to_string(record.ttl_seconds));
  AppendLenPrefixedUtf8(out, "issued_at=" + std::to_string(record.issued_at));
  AppendU32Be(out, static_cast<uint32_t>(record.multiaddrs.size()));
  for (const std::string& ma : record.multiaddrs) {
    AppendLenPrefixedUtf8(out, ma);
  }
  const uint8_t caps_present = record.capabilities.has_value() ? 1 : 0;
  out.push_back(caps_present);
  if (record.capabilities) {
    out.push_back(record.capabilities->circuit_relay ? 1 : 0);
    out.push_back(record.capabilities->media_relay ? 1 : 0);
  }
  record.ext.AppendTo(out); // last
  return out;
}

bool PeerRoutingRecordNeedsUpdate(const PeerRoutingRecordErr err) {
  return err == PeerRoutingRecordErr::NewerVersion || err == PeerRoutingRecordErr::UnknownAlgorithm ||
         err == PeerRoutingRecordErr::UnknownCritical;
}

std::string_view PeerRoutingRecordErrName(const PeerRoutingRecordErr err) {
  switch (err) {
    case PeerRoutingRecordErr::Older:
      return "older";
    case PeerRoutingRecordErr::NewerVersion:
      return "newer_version";
    case PeerRoutingRecordErr::Malformed:
      return "malformed";
    case PeerRoutingRecordErr::UnknownAlgorithm:
      return "unknown_algorithm";
    case PeerRoutingRecordErr::BadSignature:
      return "bad_signature";
    case PeerRoutingRecordErr::UnknownCritical:
      return "unknown_critical";
  }
  return "unknown";
}

PeerRoutingRecordSupport PeerRoutingRecordSupportOf(const common::Object& object) {
  if (!object.contains("version")) {
    return PeerRoutingRecordSupport::OlderVersion; // v1 always wrote 1; a record without one is older still
  }
  const std::optional<int64_t> version = object.getIf<int64_t>("version");
  if (!version) {
    return PeerRoutingRecordSupport::UnsupportedVersion; // not a number: not one this build reads
  }
  if (*version < kPeerRoutingRecordVersion) {
    return PeerRoutingRecordSupport::OlderVersion;
  }
  if (*version > kPeerRoutingRecordVersion) {
    return PeerRoutingRecordSupport::UnsupportedVersion;
  }
  return PeerRoutingRecordSupport::Supported;
}

common::Object PeerRoutingRecordToObject(const PeerRoutingRecord& record) {
  common::Object object;
  object.set("type", "peer_routing");
  object.set("version", int64_t{kPeerRoutingRecordVersion});
  object.set("peer_id", record.peer_id);
  object.set("seq", record.seq);
  object.set("ttl_seconds", record.ttl_seconds);
  object.set("issued_at", record.issued_at);
  std::vector<common::Value> mas;
  mas.reserve(record.multiaddrs.size());
  for (const std::string& ma : record.multiaddrs) {
    mas.emplace_back(ma);
  }
  object.set("multiaddrs", common::makeArray(std::move(mas)));
  if (record.capabilities) {
    common::Object caps;
    caps.set("circuit_relay", record.capabilities->circuit_relay);
    caps.set("media_relay", record.capabilities->media_relay);
    object.set("capabilities", caps);
  }
  if (!record.ext.empty()) {
    object.set("ext_b64", pp::Base64Encode(record.ext.EncodeEntries())); // writers omit an empty area
  }
  object.set("signature_b64", record.signature_b64);
  if (!record.public_key_b64.empty()) {
    object.set("public_key_b64", record.public_key_b64);
  }
  return object;
}

PeerRoutingRecordRoe<PeerRoutingRecord> PeerRoutingRecordFromObject(const common::Object& object) {
  // 1. The version, before anything else.
  switch (PeerRoutingRecordSupportOf(object)) {
    case PeerRoutingRecordSupport::UnsupportedVersion:
      return Fail(PeerRoutingRecordErr::NewerVersion, "unsupported dht record version (update needed)");
    case PeerRoutingRecordSupport::OlderVersion:
      return Fail(PeerRoutingRecordErr::Older, "dht record version below 2 (made by an older build)");
    case PeerRoutingRecordSupport::Supported:
      break;
  }
  // 2. The fields.
  PeerRoutingRecord record;
  if (object.getString("type").value_or("") != "peer_routing") {
    return Fail(PeerRoutingRecordErr::Malformed, "invalid dht record type");
  }
  record.peer_id = object.getString("peer_id").value_or("");
  if (record.peer_id.empty()) {
    return Fail(PeerRoutingRecordErr::Malformed, "missing peer_id");
  }
  record.seq = object.getIf<int64_t>("seq").value_or(0);
  record.ttl_seconds = object.getIf<int64_t>("ttl_seconds").value_or(0);
  record.issued_at = object.getIf<int64_t>("issued_at").value_or(0);
  if (const common::Array* mas = object.getArray("multiaddrs")) {
    for (const common::Value& item : mas->elements) {
      if (auto ma = common::asString(item)) {
        record.multiaddrs.push_back(*ma);
      }
    }
  }
  if (record.multiaddrs.empty()) {
    return Fail(PeerRoutingRecordErr::Malformed, "missing multiaddrs");
  }
  for (const std::string& ma : record.multiaddrs) {
    if (pp::amp::PeerIdFromMultiaddr(ma) != record.peer_id) {
      return Fail(PeerRoutingRecordErr::Malformed, "multiaddr peer_id mismatch");
    }
  }
  if (const common::Object* caps = object.getObject("capabilities")) {
    PeerRoutingCapabilities parsed_caps;
    parsed_caps.circuit_relay = caps->getIf<bool>("circuit_relay").value_or(false);
    parsed_caps.media_relay = caps->getIf<bool>("media_relay").value_or(false);
    record.capabilities = parsed_caps;
  }
  if (object.contains("ext_b64")) { // absent: the empty area
    const std::optional<std::string> ext_b64 = object.getString("ext_b64");
    if (!ext_b64) {
      return Fail(PeerRoutingRecordErr::Malformed, "ext_b64 is not a string");
    }
    auto entries = DecodeB64(*ext_b64);
    if (!entries) {
      return Fail(PeerRoutingRecordErr::Malformed, "ext_b64 is not base64");
    }
    auto area = pp::ExtArea::DecodeEntries(*entries);
    if (!area) {
      return Fail(PeerRoutingRecordErr::Malformed,
                  "ext_b64: " + std::string(pp::ExtArea::ErrName(area.error().GetCode())));
    }
    record.ext = std::move(*area);
  }
  record.signature_b64 = object.getString("signature_b64").value_or("");
  if (record.signature_b64.empty()) {
    return Fail(PeerRoutingRecordErr::Malformed, "missing signature_b64");
  }
  record.public_key_b64 = object.getString("public_key_b64").value_or("");
  // 3. The algorithms (and the typed blobs' sizes).
  if (auto signature = DecodeTypedB64(pp::BlobKind::kSignature, record.signature_b64, "signature_b64"); !signature) {
    return signature.error();
  }
  if (!record.public_key_b64.empty()) {
    if (auto key = DecodeTypedB64(pp::BlobKind::kSignaturePublicKey, record.public_key_b64, "public_key_b64"); !key) {
      return key.error();
    }
  }
  return record;
}

Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record, std::span<const uint8_t> device_signing_secret) {
  if (device_signing_secret.size() != pp::kMlDsa65SecretKeyBytes) {
    return Error("invalid signing secret size");
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  pp::ByteVector secret(device_signing_secret.begin(), device_signing_secret.end());
  auto signature = pp::MlDsa::Sign(secret, sign_bytes);
  if (!signature) {
    return signature.error();
  }
  auto typed = pp::EncodeTypedBlob(pp::BlobKind::kSignature, pp::kSignatureAlgMlDsa65, *signature);
  if (!typed) {
    return typed.error();
  }
  record.signature_b64 = pp::Base64Encode(*typed);
  return record;
}

Roe<std::string> PeerRoutingPublicKeyB64(std::span<const uint8_t> ml_dsa_65_public_key) {
  auto typed = pp::EncodeTypedBlob(pp::BlobKind::kSignaturePublicKey, pp::kSignatureAlgMlDsa65, ml_dsa_65_public_key);
  if (!typed) {
    return typed.error();
  }
  return pp::Base64Encode(*typed);
}

PeerRoutingRecordRoe<void> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                                   std::span<const uint8_t> signing_public_key) {
  if (signing_public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return Fail(PeerRoutingRecordErr::BadSignature, "signing key is not an ml-dsa-65 key");
  }
  auto signature = DecodeTypedB64(pp::BlobKind::kSignature, record.signature_b64, "signature_b64");
  if (!signature) {
    return signature.error();
  }
  if (signature->alg->id != pp::kSignatureAlgMlDsa65) {
    return Fail(PeerRoutingRecordErr::BadSignature, "signature of another algorithm than its key");
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  pp::ByteVector public_key(signing_public_key.begin(), signing_public_key.end());
  auto verified = pp::MlDsa::Verify(public_key, sign_bytes, signature->body);
  if (!verified || !*verified) {
    return Fail(PeerRoutingRecordErr::BadSignature, "bad signature");
  }
  return {};
}

PeerRoutingRecordRoe<void> VerifyPeerRoutingRecordStandalone(const PeerRoutingRecord& record) {
  if (record.public_key_b64.empty()) {
    return Fail(PeerRoutingRecordErr::Malformed, "record carries no public key");
  }
  auto key = DecodeTypedB64(pp::BlobKind::kSignaturePublicKey, record.public_key_b64, "public_key_b64");
  if (!key) {
    return key.error();
  }
  if (key->alg->id != pp::kSignatureAlgMlDsa65) { // the only key a PeerId (key type 4) derives from today
    return Fail(PeerRoutingRecordErr::BadSignature, "public key of another algorithm than its PeerId");
  }
  if (pp::amp::PeerIdFromIdentityKey(key->body) != record.peer_id) { // identities hash the raw key
    return Fail(PeerRoutingRecordErr::BadSignature, "public key does not derive the record's peer_id");
  }
  return VerifyPeerRoutingRecord(record, key->body);
}

bool PeerRoutingRecordHasUnknownCritical(const PeerRoutingRecord& record) {
  return record.ext.HasUnknownCritical(std::span<const uint16_t>{}); // no entry type is defined at release 1
}

PeerRoutingRecordRoe<PeerRoutingRecord> ReadPeerRoutingRecord(const common::Object& object) {
  auto record = PeerRoutingRecordFromObject(object);
  if (!record) {
    return record.error();
  }
  if (auto verified = VerifyPeerRoutingRecordStandalone(*record); !verified) {
    return verified.error();
  }
  if (PeerRoutingRecordHasUnknownCritical(*record)) {
    return Fail(PeerRoutingRecordErr::UnknownCritical, "unknown critical extension (update needed)");
  }
  return record;
}

std::string PeerRoutingRecordDialAddr(const PeerRoutingRecord& record) {
  for (const std::string& ma : record.multiaddrs) {
    if (ma.find("/adp/") != std::string::npos) {
      return ma;
    }
  }
  return record.multiaddrs.empty() ? std::string() : record.multiaddrs.front();
}

common::Value DhtCloserPeersToArray(const std::vector<DhtCloserPeer>& peers) {
  std::vector<common::Value> elements;
  elements.reserve(peers.size());
  for (const DhtCloserPeer& peer : peers) {
    common::Object entry;
    entry.set("peer_id", peer.peer_id);
    entry.set("multiaddr", peer.multiaddr);
    elements.emplace_back(std::make_shared<common::Object>(std::move(entry)));
  }
  return common::makeArray(std::move(elements));
}

std::vector<DhtCloserPeer> DhtCloserPeersFromResponse(const common::Object& response, const size_t max) {
  std::vector<DhtCloserPeer> out;
  const common::Array* entries = response.getArray("closer_peers");
  if (!entries) {
    return out;
  }
  for (const common::Value& item : entries->elements) {
    if (out.size() >= max) {
      break;
    }
    const common::Object* entry = common::asObject(item);
    if (!entry) {
      continue;
    }
    DhtCloserPeer peer{entry->getString("peer_id").value_or(""), entry->getString("multiaddr").value_or("")};
    if (peer.peer_id.empty() || !DhtKeyOfPeerId(peer.peer_id) ||
        pp::amp::PeerIdFromMultiaddr(peer.multiaddr) != peer.peer_id) {
      continue;
    }
    if (std::any_of(out.begin(), out.end(), [&](const DhtCloserPeer& p) { return p.peer_id == peer.peer_id; })) {
      continue;
    }
    out.push_back(std::move(peer));
  }
  return out;
}

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, const int64_t now_seconds, const int64_t grace_seconds) {
  if (record.ttl_seconds <= 0 || record.issued_at <= 0) {
    return true;
  }
  // issued_at/ttl_seconds come off the wire (peer-controlled, see AmpDhtProtocol's inbound
  // "store"); an adversarial huge value must not overflow the sum and wrap into looking
  // non-expired — treat anything that would overflow as expired (the safe default).
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  if (record.issued_at > kMax - record.ttl_seconds) {
    return true;
  }
  const int64_t expiry = record.issued_at + record.ttl_seconds;
  if (expiry > kMax - grace_seconds) {
    return true;
  }
  return now_seconds > expiry + grace_seconds;
}

MeshDirectoryNode MeshDirectoryNodeFromDhtRecord(const PeerRoutingRecord& record) {
  MeshDirectoryNode node;
  node.peer_id = record.peer_id;
  node.multiaddrs = record.multiaddrs;
  if (record.capabilities) {
    node.circuit_relay = record.capabilities->circuit_relay;
    node.media_relay = record.capabilities->media_relay;
  }
  return node;
}

std::vector<MeshDirectoryNode> MeshDirectoryNodesFromDhtRecords(const std::vector<PeerRoutingRecord>& records) {
  std::vector<MeshDirectoryNode> nodes;
  nodes.reserve(records.size());
  for (const PeerRoutingRecord& record : records) {
    if (record.peer_id.empty() || record.multiaddrs.empty()) {
      continue;
    }
    nodes.push_back(MeshDirectoryNodeFromDhtRecord(record));
  }
  return nodes;
}

} // namespace pp::mesh
