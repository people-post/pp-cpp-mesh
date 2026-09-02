#include "base/mesh/discovery/MeshNodeHitCodec.h"

#include "common/ValueJson.h"

namespace pbr {
namespace {

Object CapabilitiesToObject(const MeshCapabilitiesAd& caps) {
  Object object;
  object.set("circuit_relay", caps.circuit_relay);
  object.set("media_relay", caps.media_relay);
  object.set("dht", caps.dht);
  object.set("ledger_gateway", caps.ledger_gateway);
  return object;
}

MeshCapabilitiesAd CapabilitiesFromObject(const Object& object) {
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

Object EndpointToObject(const DirectoryEndpoint& endpoint) {
  Object object;
  object.set("peer_id", endpoint.peer_id);
  object.set("updated_at", endpoint.updated_at);
  std::vector<Value> addrs;
  addrs.reserve(endpoint.multiaddrs.size());
  for (const std::string& ma : endpoint.multiaddrs) {
    addrs.emplace_back(ma);
  }
  object.set("multiaddrs", makeArray(std::move(addrs)));
  return object;
}

} // namespace

Object MeshNodeHitToObject(const MeshNodeHit& hit) {
  Object object;
  object.set("relay_user_id", hit.relay_user_id);
  if (hit.account_id) {
    object.set("account_id", *hit.account_id);
  }
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
  object.set("capabilities", CapabilitiesToObject(hit.capabilities));
  std::vector<Value> endpoints;
  endpoints.reserve(hit.endpoints.size());
  for (const DirectoryEndpoint& ep : hit.endpoints) {
    endpoints.emplace_back(std::make_shared<Object>(EndpointToObject(ep)));
  }
  object.set("endpoints", makeArray(std::move(endpoints)));
  return object;
}

Roe<MeshNodeHit> MeshNodeHitFromObject(const Object& object) {
  MeshNodeHit hit;
  if (auto relay = object.getString("relay_user_id")) {
    hit.relay_user_id = *relay;
  }
  if (hit.relay_user_id.empty()) {
    return Error("mesh node missing relay_user_id");
  }
  if (auto account = object.getString("account_id")) {
    hit.account_id = *account;
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
  if (auto pk = object.getString("signing_public_key_b64")) {
    hit.signing_public_key_b64 = *pk;
  }
  if (auto kem = object.getString("kem_public_key_b64")) {
    hit.kem_public_key_b64 = *kem;
  }
  if (const Object* caps = object.getObject("capabilities")) {
    hit.capabilities = CapabilitiesFromObject(*caps);
  }
  if (const Array* endpoints = object.getArray("endpoints")) {
    for (const Value& ep_val : endpoints->elements) {
      const Object* ep = asObject(ep_val);
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
      if (const Array* mas = ep->getArray("multiaddrs")) {
        for (const Value& ma : mas->elements) {
          if (auto s = asString(ma)) {
            row.multiaddrs.push_back(*s);
          }
        }
      }
      hit.endpoints.push_back(std::move(row));
    }
  }
  return hit;
}

Value MeshNodeHitsToJsonArray(const std::vector<MeshNodeHit>& hits) {
  std::vector<Value> elements;
  elements.reserve(hits.size());
  for (const MeshNodeHit& hit : hits) {
    elements.emplace_back(std::make_shared<Object>(MeshNodeHitToObject(hit)));
  }
  return makeArray(std::move(elements));
}

Roe<std::vector<MeshNodeHit>> MeshNodeHitsFromJsonArray(const Array& nodes) {
  std::vector<MeshNodeHit> out;
  out.reserve(nodes.elements.size());
  for (const Value& item : nodes.elements) {
    const Object* obj = asObject(item);
    if (!obj) {
      continue;
    }
    if (auto hit = MeshNodeHitFromObject(*obj)) {
      out.push_back(std::move(*hit));
    }
  }
  return out;
}

} // namespace pbr
