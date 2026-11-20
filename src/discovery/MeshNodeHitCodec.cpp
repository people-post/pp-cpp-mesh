#include "mesh/discovery/MeshNodeHitCodec.h"

#include "common/ExtArea.h"
#include "common/ValueJson.h"
#include "crypto/AlgId.h"
#include "crypto/SodiumUtil.h"

#include <optional>
#include <span>
namespace pp::mesh {
namespace {

DirectoryKeyShape TypedBlobShape(const pp::BlobKind kind, std::span<const uint8_t> bytes) {
  auto blob = pp::DecodeTypedBlob(kind, bytes);
  if (blob) {
    return DirectoryKeyShape::Ok;
  }
  return pp::IsUnknownAlgorithm(blob.error()) ? DirectoryKeyShape::UpdateNeeded : DirectoryKeyShape::Malformed;
}

/** Empty or not base64: nullopt (pp::Base64Decode must not be given an empty string). */
std::optional<pp::ByteVector> DecodeB64(const std::string& b64) {
  if (b64.empty()) {
    return std::nullopt;
  }
  auto bytes = pp::Base64Decode(b64);
  if (!bytes || bytes->empty()) {
    return std::nullopt;
  }
  return std::move(*bytes);
}

/** A key field as read: kept (and flagged when update needed) or dropped. */
std::optional<std::string> ReadKeyField(const common::Object& object, const std::string& field,
                                        DirectoryKeyShape (*shape_of)(const std::string&), bool& update_needed) {
  std::optional<std::string> value = object.getString(field);
  if (!value) {
    return std::nullopt;
  }
  switch (shape_of(*value)) {
    case DirectoryKeyShape::Ok:
      return value;
    case DirectoryKeyShape::UpdateNeeded:
      update_needed = true;
      return value;
    case DirectoryKeyShape::Older:
    case DirectoryKeyShape::Malformed:
      return std::nullopt;
  }
  return std::nullopt;
}

common::Object CapabilitiesToObject(const MeshCapabilitiesAd& caps) {
  common::Object object;
  object.set("circuit_relay", caps.circuit_relay);
  object.set("media_relay", caps.media_relay);
  object.set("dht", caps.dht);
  object.set("ledger_gateway", caps.ledger_gateway);
  return object;
}

MeshCapabilitiesAd CapabilitiesFromObject(const common::Object& object) {
  MeshCapabilitiesAd caps;
  if (auto circuit = object.getIf<bool>("circuit_relay")) {
    caps.circuit_relay = *circuit;
  }
  if (auto media = object.getIf<bool>("media_relay")) {
    caps.media_relay = *media;
  }
  if (auto dht = object.getIf<bool>("dht")) {
    caps.dht = *dht;
  }
  if (auto ledger = object.getIf<bool>("ledger_gateway")) {
    caps.ledger_gateway = *ledger;
  }
  return caps;
}

common::Object EndpointToObject(const DirectoryEndpoint& endpoint) {
  common::Object object;
  object.set("peer_id", endpoint.peer_id);
  object.set("updated_at", endpoint.updated_at);
  std::vector<common::Value> addrs;
  addrs.reserve(endpoint.multiaddrs.size());
  for (const std::string& ma : endpoint.multiaddrs) {
    addrs.emplace_back(ma);
  }
  object.set("multiaddrs", common::makeArray(std::move(addrs)));
  return object;
}

} // namespace

common::Object MeshNodeHitToObject(const MeshNodeHit& hit) {
  common::Object object;
  object.set("account_id", hit.account_id);
  if (hit.nickname) {
    object.set("nickname", *hit.nickname);
  }
  if (!hit.expires_at.empty()) {
    object.set("expires_at", hit.expires_at);
  }
  object.set("entity_kind", hit.entity_kind.empty() ? "mesh_node" : hit.entity_kind);
  object.set("seq", hit.seq);
  if (hit.signing_public_key_b64) {
    object.set("signing_public_key_b64", *hit.signing_public_key_b64);
  }
  if (hit.kem_public_key_b64) {
    object.set("kem_public_key_b64", *hit.kem_public_key_b64);
  }
  if (hit.kem_binding_b64) {
    object.set("kem_binding_b64", *hit.kem_binding_b64);
  }
  object.set("capabilities", CapabilitiesToObject(hit.capabilities));
  if (hit.amp_wire > 0) {
    object.set("amp_wire", static_cast<int64_t>(hit.amp_wire));
  }
  if (!hit.node_version.empty()) {
    object.set("node_version", hit.node_version);
  }
  if (!hit.features.empty()) {
    std::vector<common::Value> features;
    for (const std::string& f : hit.features) {
      features.emplace_back(f);
    }
    object.set("features", common::makeArray(std::move(features)));
  }
  std::vector<common::Value> endpoints;
  endpoints.reserve(hit.endpoints.size());
  for (const DirectoryEndpoint& ep : hit.endpoints) {
    endpoints.emplace_back(std::make_shared<common::Object>(EndpointToObject(ep)));
  }
  object.set("endpoints", common::makeArray(std::move(endpoints)));
  return object;
}

Roe<MeshNodeHit> MeshNodeHitFromObject(const common::Object& object) {
  MeshNodeHit hit;
  if (auto account = object.getString("account_id")) {
    hit.account_id = *account;
  }
  if (hit.account_id.empty()) {
    return Error("mesh node missing account_id");
  }
  if (auto nick = object.getString("nickname")) {
    hit.nickname = *nick;
  }
  if (auto expires = object.getString("expires_at")) {
    hit.expires_at = *expires;
  }
  if (auto kind = object.getString("entity_kind")) {
    hit.entity_kind = *kind;
  }
  if (hit.entity_kind.empty()) {
    hit.entity_kind = "mesh_node";
  }
  if (auto seq = object.getIf<int64_t>("seq")) {
    hit.seq = *seq;
  }
  hit.signing_public_key_b64 =
      ReadKeyField(object, "signing_public_key_b64", SigningPublicKeyShape, hit.keys_update_needed);
  hit.kem_public_key_b64 = ReadKeyField(object, "kem_public_key_b64", KemPublicKeyShape, hit.keys_update_needed);
  hit.kem_binding_b64 = ReadKeyField(object, "kem_binding_b64", KemBindingShape, hit.keys_update_needed);
  if (const common::Object* caps = object.getObject("capabilities")) {
    hit.capabilities = CapabilitiesFromObject(*caps);
  }
  if (auto wire = object.getIf<int64_t>("amp_wire"); wire && *wire > 0 && *wire < 16) {
    hit.amp_wire = static_cast<int>(*wire);
  }
  if (auto version = object.getString("node_version")) {
    hit.node_version = *version;
  }
  if (const common::Array* features = object.getArray("features")) {
    for (const common::Value& f : features->elements) {
      if (auto s = common::asString(f)) {
        hit.features.push_back(*s);
      }
    }
  }
  if (const common::Array* endpoints = object.getArray("endpoints")) {
    for (const common::Value& ep_val : endpoints->elements) {
      const common::Object* ep = common::asObject(ep_val);
      if (!ep) {
        continue;
      }
      DirectoryEndpoint row;
      if (auto peer = ep->getString("peer_id")) {
        row.peer_id = *peer;
      }
      if (row.peer_id.empty()) {
        continue;
      }
      if (auto updated = ep->getIf<int64_t>("updated_at")) {
        row.updated_at = *updated;
      }
      if (const common::Array* mas = ep->getArray("multiaddrs")) {
        for (const common::Value& ma : mas->elements) {
          if (auto s = common::asString(ma)) {
            row.multiaddrs.push_back(*s);
          }
        }
      }
      hit.endpoints.push_back(std::move(row));
    }
  }
  return hit;
}

common::Value MeshNodeHitsToJsonArray(const std::vector<MeshNodeHit>& hits) {
  std::vector<common::Value> elements;
  elements.reserve(hits.size());
  for (const MeshNodeHit& hit : hits) {
    elements.emplace_back(std::make_shared<common::Object>(MeshNodeHitToObject(hit)));
  }
  return common::makeArray(std::move(elements));
}

Roe<std::vector<MeshNodeHit>> MeshNodeHitsFromJsonArray(const common::Array& nodes) {
  std::vector<MeshNodeHit> out;
  out.reserve(nodes.elements.size());
  for (const common::Value& item : nodes.elements) {
    const common::Object* obj = common::asObject(item);
    if (!obj) {
      continue;
    }
    if (auto hit = MeshNodeHitFromObject(*obj)) {
      out.push_back(std::move(*hit));
    }
  }
  return out;
}

DirectoryKeyShape SigningPublicKeyShape(const std::string& b64) {
  auto bytes = DecodeB64(b64);
  return bytes ? TypedBlobShape(pp::BlobKind::kSignaturePublicKey, *bytes) : DirectoryKeyShape::Malformed;
}

DirectoryKeyShape KemPublicKeyShape(const std::string& b64) {
  auto bytes = DecodeB64(b64);
  return bytes ? TypedBlobShape(pp::BlobKind::kKemPublicKey, *bytes) : DirectoryKeyShape::Malformed;
}

DirectoryKeyShape KemBindingShape(const std::string& b64) {
  auto bytes = DecodeB64(b64);
  if (!bytes) {
    return DirectoryKeyShape::Malformed;
  }
  // 1. The version, first.
  const uint8_t version = (*bytes)[0];
  if (version < kKemBindingVersion) {
    return DirectoryKeyShape::Older;
  }
  if (version > kKemBindingVersion) {
    return DirectoryKeyShape::UpdateNeeded; // nothing after it is read
  }
  // 2. The area, then the signature's length: exactly the rest.
  size_t offset = 1;
  if (!pp::ExtArea::DecodeAt(*bytes, &offset)) {
    return DirectoryKeyShape::Malformed;
  }
  if (bytes->size() - offset < 8) {
    return DirectoryKeyShape::Malformed;
  }
  uint64_t length = 0;
  for (int i = 0; i < 8; ++i) {
    length = (length << 8) | (*bytes)[offset + static_cast<size_t>(i)];
  }
  offset += 8;
  if (length != bytes->size() - offset) {
    return DirectoryKeyShape::Malformed; // past the end, or bytes left over
  }
  // 3. The signature's algorithm (and size).
  return TypedBlobShape(pp::BlobKind::kSignature, std::span<const uint8_t>(*bytes).subspan(offset));
}

} // namespace pp::mesh
