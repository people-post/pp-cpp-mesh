#include "base/mesh/dht/DhtRecordCodec.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/MlDsa.h"
#include "base/data/MeshRole.h"
#include "common/Utilities.h"

#include <cstring>
#include <string_view>

namespace pbr {
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
  return out;
}

Object PeerRoutingRecordToObject(const PeerRoutingRecord& record) {
  Object object;
  object.set("type", "peer_routing");
  object.set("version", int64_t{kDhtWireVersion});
  object.set("peer_id", record.peer_id);
  object.set("seq", record.seq);
  object.set("ttl_seconds", record.ttl_seconds);
  object.set("issued_at", record.issued_at);
  std::vector<Value> mas;
  mas.reserve(record.multiaddrs.size());
  for (const std::string& ma : record.multiaddrs) {
    mas.emplace_back(ma);
  }
  object.set("multiaddrs", makeArray(std::move(mas)));
  object.set("signature_b64", record.signature_b64);
  object.set("signature_alg", record.signature_alg);
  return object;
}

Roe<PeerRoutingRecord> PeerRoutingRecordFromObject(const Object& object) {
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
  if (const Array* mas = object.getArray("multiaddrs")) {
    for (const Value& item : mas->elements) {
      if (auto ma = asString(item)) {
        record.multiaddrs.push_back(*ma);
      }
    }
  }
  if (record.multiaddrs.empty()) {
    return Error("missing multiaddrs");
  }
  for (const std::string& ma : record.multiaddrs) {
    if (PeerIdFromMultiaddr(ma) != record.peer_id) {
      return Error("multiaddr peer_id mismatch");
    }
  }
  return record;
}

Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record,
                                             std::span<const uint8_t> device_signing_secret) {
  if (device_signing_secret.size() != kMlDsa65SecretKeyBytes) {
    return Error("invalid signing secret size");
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  ByteVector secret(device_signing_secret.begin(), device_signing_secret.end());
  auto signature = MlDsa::Sign(secret, sign_bytes);
  if (!signature) {
    return signature.error();
  }
  record.signature_b64 = Base64Encode(*signature);
  record.signature_alg = "ml-dsa-65";
  return record;
}

Roe<bool> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                  std::span<const uint8_t> signing_public_key) {
  if (signing_public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("invalid signing public key size");
  }
  if (record.signature_alg != "ml-dsa-65") {
    return Error("unsupported signature_alg");
  }
  auto signature = Base64Decode(record.signature_b64);
  if (!signature) {
    return signature.error();
  }
  const auto sign_bytes = BuildPeerRoutingSignBytes(record);
  ByteVector public_key(signing_public_key.begin(), signing_public_key.end());
  return MlDsa::Verify(public_key, sign_bytes, *signature);
}

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, const int64_t now_seconds,
                              const int64_t grace_seconds) {
  if (record.ttl_seconds <= 0 || record.issued_at <= 0) {
    return true;
  }
  return now_seconds > record.issued_at + record.ttl_seconds + grace_seconds;
}

} // namespace pbr
