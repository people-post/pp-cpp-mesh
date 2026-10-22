#include "mesh/dht/DhtRecordCodec.h"

#include "mesh/dht/DhtKey.h"

#include "amp/link/PeerId.h"
#include "common/Utilities.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"

#include <algorithm>
#include <cstring>
#include <limits>
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

} // namespace

std::vector<uint8_t> BuildPeerRoutingSignBytes(const PeerRoutingRecord& record) {
  std::vector<uint8_t> out;
  constexpr char kDomain[] = "pp-mesh:dht-record-v1";
  out.insert(out.end(), kDomain, kDomain + sizeof(kDomain));
  AppendLenPrefixedUtf8(out, "sign_version=1");
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
  return out;
}

common::Object PeerRoutingRecordToObject(const PeerRoutingRecord& record) {
  common::Object object;
  object.set("type", "peer_routing");
  object.set("version", int64_t{kDhtWireVersion});
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
  object.set("signature_b64", record.signature_b64);
  object.set("signature_alg", record.signature_alg);
  if (!record.public_key_b64.empty()) {
    object.set("public_key_b64", record.public_key_b64);
  }
  return object;
}

Roe<PeerRoutingRecord> PeerRoutingRecordFromObject(const common::Object& object) {
  PeerRoutingRecord record;
  if (object.getString("type").value_or("") != "peer_routing") {
    return Error("invalid dht record type");
  }
  record.peer_id = object.getString("peer_id").value_or("");
  if (record.peer_id.empty()) {
    return Error("missing peer_id");
  }
  record.seq = object.getIf<int64_t>("seq").value_or(0);
  record.ttl_seconds = object.getIf<int64_t>("ttl_seconds").value_or(0);
  record.issued_at = object.getIf<int64_t>("issued_at").value_or(0);
  record.signature_b64 = object.getString("signature_b64").value_or("");
  record.signature_alg = object.getString("signature_alg").value_or("ml-dsa-65");
  record.public_key_b64 = object.getString("public_key_b64").value_or("");
  if (const common::Array* mas = object.getArray("multiaddrs")) {
    for (const common::Value& item : mas->elements) {
      if (auto ma = common::asString(item)) {
        record.multiaddrs.push_back(*ma);
      }
    }
  }
  if (record.multiaddrs.empty()) {
    return Error("missing multiaddrs");
  }
  for (const std::string& ma : record.multiaddrs) {
    if (pp::amp::PeerIdFromMultiaddr(ma) != record.peer_id) {
      return Error("multiaddr peer_id mismatch");
    }
  }
  if (const common::Object* caps = object.getObject("capabilities")) {
    PeerRoutingCapabilities parsed_caps;
    parsed_caps.circuit_relay = caps->getIf<bool>("circuit_relay").value_or(false);
    parsed_caps.media_relay = caps->getIf<bool>("media_relay").value_or(false);
    record.capabilities = parsed_caps;
  }
  return record;
}

Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record,
                                             std::span<const uint8_t> device_signing_secret) {
  if (device_signing_secret.size() != pp::kMlDsa65SecretKeyBytes) {
    return Error("invalid signing secret size");
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  pp::ByteVector secret(device_signing_secret.begin(), device_signing_secret.end());
  auto signature = pp::MlDsa::Sign(secret, sign_bytes);
  if (!signature) {
    return signature.error();
  }
  record.signature_b64 = pp::Base64Encode(*signature);
  record.signature_alg = "ml-dsa-65";
  return record;
}

Roe<bool> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                  std::span<const uint8_t> signing_public_key) {
  if (signing_public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return Error("invalid signing public key size");
  }
  if (record.signature_alg != "ml-dsa-65") {
    return Error("unsupported signature_alg");
  }
  auto signature = pp::Base64Decode(record.signature_b64);
  if (!signature) {
    return signature.error();
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  pp::ByteVector public_key(signing_public_key.begin(), signing_public_key.end());
  return pp::MlDsa::Verify(public_key, sign_bytes, *signature);
}

Roe<void> VerifyPeerRoutingRecordStandalone(const PeerRoutingRecord& record) {
  if (record.public_key_b64.empty()) {
    return Error("record carries no public key");
  }
  auto public_key = pp::Base64Decode(record.public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  if (pp::amp::PeerIdFromIdentityKey(*public_key) != record.peer_id) {
    return Error("public key does not derive the record's peer_id");
  }
  auto verified = VerifyPeerRoutingRecord(record, *public_key);
  if (!verified) {
    return verified.error();
  }
  if (!*verified) {
    return Error("bad signature");
  }
  return {};
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

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, const int64_t now_seconds,
                              const int64_t grace_seconds) {
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

std::vector<MeshDirectoryNode> MeshDirectoryNodesFromDhtRecords(
    const std::vector<PeerRoutingRecord>& records) {
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
