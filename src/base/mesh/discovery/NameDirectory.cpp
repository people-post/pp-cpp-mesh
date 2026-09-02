#include "base/mesh/discovery/NameDirectory.h"

#include "foundation/data/Config.h"

namespace pbr {
namespace {

std::string PreferAccountName(const DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return hit.hit_id;
}

} // namespace

NameCapabilities NameCapabilitiesFromAd(const MeshCapabilitiesAd& ad) {
  NameCapabilities caps;
  caps.circuit_relay = ad.circuit_relay;
  caps.media_relay = ad.media_relay;
  caps.dht = ad.dht;
  caps.ledger_gateway = ad.ledger_gateway;
  return caps;
}

MeshCapabilitiesAd MeshCapabilitiesAdFromName(const NameCapabilities& caps) {
  MeshCapabilitiesAd ad;
  ad.circuit_relay = caps.circuit_relay;
  ad.media_relay = caps.media_relay;
  ad.dht = caps.dht;
  ad.ledger_gateway = caps.ledger_gateway;
  return ad;
}

NameRecord NameRecordFromMeshNodeHit(const MeshNodeHit& hit) {
  NameRecord record;
  record.relay_user_id = hit.relay_user_id;
  record.account_id = hit.account_id.value_or("");
  record.name = !record.account_id.empty() ? record.account_id : hit.relay_user_id;
  record.nickname = hit.nickname.value_or("");
  record.entity_kind = hit.entity_kind.empty() ? "mesh_node" : hit.entity_kind;
  record.capabilities = NameCapabilitiesFromAd(hit.capabilities);
  record.seq = hit.seq;
  record.expires_at = hit.expires_at;
  record.endpoints = hit.endpoints;
  if (!hit.endpoints.empty()) {
    record.peer_id = hit.endpoints.front().peer_id;
    record.multiaddrs = hit.endpoints.front().multiaddrs;
  }
  return record;
}

NameRecord NameRecordFromDirectoryHit(const DirectoryHit& hit) {
  NameRecord record;
  record.name = PreferAccountName(hit);
  record.account_id = hit.account_id.value_or(record.name);
  record.nickname = !hit.nickname.empty() ? hit.nickname : hit.display_name;
  record.entity_kind = hit.entity_kind.empty() ? "person" : hit.entity_kind;
  record.capabilities.circuit_relay = hit.circuit_relay;
  record.capabilities.media_relay = hit.media_relay;
  record.capabilities.dht = hit.dht;
  record.capabilities.ledger_gateway = hit.ledger_gateway;
  record.seq = hit.seq;
  record.expires_at = hit.expires_at;
  record.endpoints = hit.endpoints;
  record.multiaddrs = hit.multiaddrs;
  record.relay_user_id = hit.hit_id;
  if (!hit.endpoints.empty()) {
    record.peer_id = hit.endpoints.front().peer_id;
  } else {
    for (const ContactId& id : hit.ids) {
      if (id.kind == ContactIdKind::PeerId && !id.value.empty()) {
        record.peer_id = id.value;
        break;
      }
    }
  }
  return record;
}

std::vector<NameRecord> NameRecordsFromMeshNodeHits(const std::vector<MeshNodeHit>& hits) {
  std::vector<NameRecord> out;
  out.reserve(hits.size());
  for (const MeshNodeHit& hit : hits) {
    out.push_back(NameRecordFromMeshNodeHit(hit));
  }
  return out;
}

std::vector<MeshDirectoryNode> MeshDirectoryNodesFromNameRecords(const std::vector<NameRecord>& records) {
  std::vector<MeshDirectoryNode> out;
  for (const NameRecord& record : records) {
    if (!record.endpoints.empty()) {
      for (const DirectoryEndpoint& ep : record.endpoints) {
        if (ep.peer_id.empty()) {
          continue;
        }
        MeshDirectoryNode node;
        node.peer_id = ep.peer_id;
        node.multiaddrs = ep.multiaddrs;
        node.circuit_relay = record.capabilities.circuit_relay;
        node.media_relay = record.capabilities.media_relay;
        node.dht = record.capabilities.dht;
        node.ledger_gateway = record.capabilities.ledger_gateway;
        node.account_id = record.account_id;
        node.entity_kind = record.entity_kind;
        node.seq = record.seq;
        node.expires_at = record.expires_at;
        node.nickname = record.nickname;
        out.push_back(std::move(node));
      }
      continue;
    }
    if (record.peer_id.empty()) {
      continue;
    }
    MeshDirectoryNode node;
    node.peer_id = record.peer_id;
    node.multiaddrs = record.multiaddrs;
    node.circuit_relay = record.capabilities.circuit_relay;
    node.media_relay = record.capabilities.media_relay;
    node.dht = record.capabilities.dht;
    node.ledger_gateway = record.capabilities.ledger_gateway;
    node.account_id = record.account_id;
    node.entity_kind = record.entity_kind;
    node.seq = record.seq;
    node.expires_at = record.expires_at;
    node.nickname = record.nickname;
    out.push_back(std::move(node));
  }
  return out;
}

DirectoryClientNameDirectory::DirectoryClientNameDirectory(IDirectoryClient& directory) : directory_(directory) {}

Roe<NameRecord> DirectoryClientNameDirectory::Resolve(const std::string& name) {
  if (name.empty()) {
    return Error("name directory: empty name");
  }
  if (name.rfind("account:", 0) == 0) {
    auto hit = directory_.LookupByAccount(name);
    if (!hit) {
      return hit.error();
    }
    return NameRecordFromDirectoryHit(*hit);
  }
  if (name.rfind("relay:", 0) == 0) {
    auto hit = directory_.LookupRelayUser(name);
    if (!hit) {
      return hit.error();
    }
    return NameRecordFromDirectoryHit(*hit);
  }
  // Treat bare strings as Account IDs without the prefix when LookupByAccount requires it.
  const std::string account = name.rfind("account:", 0) == 0 ? name : ("account:" + name);
  auto hit = directory_.LookupByAccount(account);
  if (!hit) {
    return hit.error();
  }
  return NameRecordFromDirectoryHit(*hit);
}

Roe<std::vector<NameRecord>> DirectoryClientNameDirectory::ListService(const std::string& kind) {
  const std::string service = kind.empty() ? "mesh_node" : kind;
  if (service != "mesh_node") {
    return std::vector<NameRecord>{};
  }
  auto hits = directory_.ListMeshNodes();
  if (!hits) {
    return hits.error();
  }
  return NameRecordsFromMeshNodeHits(*hits);
}

} // namespace pbr
