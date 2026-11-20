// Frozen DHT record vectors (pp-browser projects/release-compatibility/SIGNED_FORMATS § 6 "Per object", § 2.10; RC020),
// in data/dht_record_vectors.json:
//
//  - the signed bytes of a fixed `PeerRoutingRecord` v2 with an empty extension area, and with one unknown
//    non-critical entry (hex): a change to the signed bytes without a version bump fails here;
//  - signed records (the device key of pp-cpp-crypto's seeded keygen vectors, seed 0x0b…): one that verifies, and one
//    with the unknown non-critical entry that verifies with the entry kept;
//  - refusals, each with the step of the receiver's order (SIGNED_FORMATS § 1.1) that refuses it: version + 1 (update
//    needed, before anything is verified), version − 1 and none (older), an unknown critical entry (verifies, then
//    update needed), an unknown algorithm id in the signature or the key (update needed), id 0 and a bad area
//    (malformed), a changed field and a key that is not the subject's (forgery).
//
// ML-DSA signing is hedged, so signatures are frozen whole: the test rebuilds everything else (the signer, the input,
// the signed bytes, every record without its signature) and requires it to equal the file, then reads every record.
// PP_WRITE_DHT_RECORD_VECTORS=<path> writes a fresh file (new signatures, everything else the same): only with a new
// case or a new record version, never to make changed signed bytes pass.

#include "mesh/dht/DhtRecordCodec.h"

#include "amp/link/PeerId.h"
#include "common/ExtArea.h"
#include "common/ValueJson.h"
#include "crypto/MlDsa.h"
#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

using common::Object;
using common::Value;

constexpr const char* kSignerSeedHex = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b";
/** pp-cpp-crypto v1.4.0 tests/data/seeded_keygen_vectors.json, role `device`. */
constexpr const char* kSignerPublicKeySha256Hex = "a4e955b98638e8e04f668e7fa707398013a481897bd51da92947784507b258b2";
constexpr const char* kSignerPeerId = "QmSeCALxg91P6cB5CT6Tx19gYZSyeP5q6QHHRMhy7AAYsT";
/** Operator 1's seed there: a valid ML-DSA-65 key that is not the subject's. */
constexpr const char* kOtherSeedHex = "0101010101010101010101010101010101010101010101010101010101010101";

std::string Sha256Hex(const ByteVector& data) {
  ByteVector digest(crypto_hash_sha256_BYTES);
  crypto_hash_sha256(digest.data(), data.data(), data.size());
  return BytesToHex(digest);
}

MlDsaKeyPair KeysFromSeed(const char* seed_hex) {
  auto seed = HexToBytes(seed_hex);
  EXPECT_TRUE(static_cast<bool>(seed));
  auto keys = MlDsa::GenerateKeyPairFromSeed(seed ? *seed : ByteVector{});
  EXPECT_TRUE(static_cast<bool>(keys));
  return keys ? *keys : MlDsaKeyPair{};
}

PeerRoutingRecord FixedInput() {
  PeerRoutingRecord record;
  record.peer_id = kSignerPeerId;
  record.seq = 7;
  record.ttl_seconds = 3600;
  record.issued_at = 1'767'225'600; // 2026-01-01T00:00:00Z
  record.multiaddrs = {std::string("/ip4/203.0.113.10/udp/443/adp/2/p2p/") + kSignerPeerId,
                       std::string("/ip6/2001:db8::10/udp/443/adp/2/p2p/") + kSignerPeerId};
  record.capabilities = PeerRoutingCapabilities{.circuit_relay = true, .media_relay = false};
  return record;
}

ExtArea AreaOf(std::vector<ExtArea::Entry> entries) {
  auto area = ExtArea::FromEntries(std::move(entries));
  EXPECT_TRUE(static_cast<bool>(area));
  return area ? *area : ExtArea{};
}

/** pp-cpp-common's ext area vectors: `one_non_critical` (type 0x0001) and `one_critical` (type 0x8001). */
ExtArea NonCriticalArea() {
  return AreaOf({{0x0001, {0x01, 0x02, 0x03}}});
}
ExtArea CriticalArea() {
  return AreaOf({{0x8001, {0xaa, 0xbb}}});
}

/** The receiver's class of a refusal (SIGNED_FORMATS § 1.1). */
std::string ResultOf(const PeerRoutingRecordErr err) {
  switch (err) {
    case PeerRoutingRecordErr::Older:
      return "older";
    case PeerRoutingRecordErr::Malformed:
      return "malformed";
    case PeerRoutingRecordErr::BadSignature:
      return "forgery";
    case PeerRoutingRecordErr::NewerVersion:
    case PeerRoutingRecordErr::UnknownAlgorithm:
    case PeerRoutingRecordErr::UnknownCritical:
      return "update_needed";
  }
  return "?";
}

int64_t StepOf(const PeerRoutingRecordErr err) {
  switch (err) {
    case PeerRoutingRecordErr::Older:
    case PeerRoutingRecordErr::NewerVersion:
      return 1;
    case PeerRoutingRecordErr::Malformed:
      return 2;
    case PeerRoutingRecordErr::UnknownAlgorithm:
      return 3;
    case PeerRoutingRecordErr::BadSignature:
      return 4;
    case PeerRoutingRecordErr::UnknownCritical:
      return 5;
  }
  return 0;
}

/** A typed blob in base64 with its algorithm id byte replaced. */
std::string WithAlgId(const std::string& typed_b64, const uint8_t id) {
  auto bytes = Base64Decode(typed_b64);
  EXPECT_TRUE(bytes && !bytes->empty());
  if (!bytes || bytes->empty()) {
    return typed_b64;
  }
  (*bytes)[0] = id;
  return Base64Encode(*bytes);
}

Object SignedObject(PeerRoutingRecord record, const MlDsaKeyPair& keys) {
  auto signed_record = SignPeerRoutingRecord(std::move(record), keys.secret_key);
  EXPECT_TRUE(static_cast<bool>(signed_record));
  auto key = PeerRoutingPublicKeyB64(keys.public_key);
  EXPECT_TRUE(static_cast<bool>(key));
  if (!signed_record || !key) {
    return Object{};
  }
  signed_record->public_key_b64 = *key;
  return PeerRoutingRecordToObject(*signed_record);
}

Value Case(const std::string& name, const std::string& about, const Object& record, const std::string& result,
           const std::string& refusal = "", const int64_t step = 0) {
  Object row;
  row.set("name", name);
  row.set("about", about);
  row.set("record", common::ObjectValue(record));
  row.set("result", result);
  if (!refusal.empty()) {
    row.set("refusal", refusal);
    row.set("step", step);
  }
  return common::ObjectValue(std::move(row));
}

Value SignedBytesRow(const std::string& name, const std::string& about, const ExtArea& ext) {
  PeerRoutingRecord record = FixedInput();
  record.ext = ext;
  Object row;
  row.set("name", name);
  row.set("about", about);
  row.set("ext_b64", Base64Encode(ext.EncodeEntries()));
  row.set("signed_bytes_hex", BytesToHex(BuildPeerRoutingSignBytes(record)));
  return common::ObjectValue(std::move(row));
}

Object BuildVectors() {
  const MlDsaKeyPair keys = KeysFromSeed(kSignerSeedHex);
  const MlDsaKeyPair other = KeysFromSeed(kOtherSeedHex);

  Object root;
  root.set("format", "pp-cpp-mesh dht record vectors v1");
  root.set(
      "about",
      "PeerRoutingRecord v2 (pp-browser SIGNED_FORMATS 2.10, RC020). signed = \"pp-mesh:dht-record\" 00 || u8 2 || "
      "Len(\"type=peer_routing\") || Len(peer_id) || Len(\"seq=\"dec) || Len(\"ttl_seconds=\"dec) || "
      "Len(\"issued_at=\"dec) || u32 n || n x Len(multiaddr) || u8 caps_present [|| u8 circuit_relay || u8 "
      "media_relay] || ext area (u64 BE length || entries); Len = u32 BE length then UTF-8. JSON: version 2 read "
      "first, ext_b64 (absent when empty), typed signature_b64 and public_key_b64 (u8 alg id || body; 1 = "
      "ml-dsa-65). Signatures are hedged, frozen whole. result: ok | older | update_needed | malformed | forgery; "
      "step: of the receiver's order (1 version, 2 parse, 3 algorithms, 4 signature, 5 extensions).");
  root.set("domain", std::string(kPeerRoutingRecordDomain));
  root.set("version", int64_t{kPeerRoutingRecordVersion});

  Object signer;
  signer.set("role", "device");
  signer.set("seed_hex", kSignerSeedHex);
  signer.set("public_key_sha256_hex", Sha256Hex(keys.public_key));
  signer.set("peer_id", pp::amp::PeerIdFromIdentityKey(keys.public_key));
  root.set("signer", common::ObjectValue(std::move(signer)));

  const PeerRoutingRecord input = FixedInput();
  Object input_obj = PeerRoutingRecordToObject(input);
  input_obj.erase("signature_b64");
  root.set("input", common::ObjectValue(std::move(input_obj)));

  root.set("signed_bytes",
           common::makeArray({
               SignedBytesRow("empty_area", "the input with the empty area (every record at release 1)", ExtArea{}),
               SignedBytesRow("one_non_critical", "the input with one unknown non-critical entry, type 0x0001",
                              NonCriticalArea()),
           }));

  PeerRoutingRecord with_entry = input;
  with_entry.ext = NonCriticalArea();
  PeerRoutingRecord with_critical = input;
  with_critical.ext = CriticalArea();
  const Object signed_obj = SignedObject(input, keys);

  Object newer = signed_obj;
  newer.set("version", int64_t{kPeerRoutingRecordVersion + 1});
  Object older = signed_obj;
  older.set("version", int64_t{kPeerRoutingRecordVersion - 1});
  Object no_version = signed_obj;
  no_version.erase("version");
  Object sig_alg = signed_obj;
  sig_alg.set("signature_b64", WithAlgId(signed_obj.getString("signature_b64").value_or(""), 2));
  Object key_alg = signed_obj;
  key_alg.set("public_key_b64", WithAlgId(signed_obj.getString("public_key_b64").value_or(""), 2));
  Object sig_zero = signed_obj;
  sig_zero.set("signature_b64", WithAlgId(signed_obj.getString("signature_b64").value_or(""), 0));
  Object bad_area = signed_obj;
  bad_area.set("ext_b64", "AAIAAAAAAAEAAAAA"); // types 0x0002 then 0x0001, empty values
  Object changed = signed_obj;
  changed.set("seq", int64_t{8});
  Object wrong_key = signed_obj;
  wrong_key.set("public_key_b64", PeerRoutingPublicKeyB64(other.public_key).valueOr(""));

  root.set(
      "records",
      common::makeArray({
          Case("signed", "the input, empty area, signed: verifies", signed_obj, "ok"),
          Case("signed_one_non_critical",
               "the input with the unknown non-critical entry, signed: verifies, the entry kept (ext_b64 unchanged)",
               SignedObject(with_entry, keys), "ok"),
          Case("version_plus_one", "signed's JSON with version 3: update needed, nothing verified", newer,
               "update_needed", "newer_version", 1),
          Case("version_minus_one", "signed's JSON with version 1: made by an older build", older, "older", "older", 1),
          Case("version_missing", "signed's JSON without version: made by an older build (v1 always wrote one)",
               no_version, "older", "older", 1),
          Case("unknown_critical", "the input with a critical entry, type 0x8001, signed: verifies, then update needed",
               SignedObject(with_critical, keys), "update_needed", "unknown_critical", 5),
          Case("unknown_signature_algorithm", "signed with the signature's id byte 2 (unassigned): update needed",
               sig_alg, "update_needed", "unknown_algorithm", 3),
          Case("unknown_key_algorithm", "signed with the key's id byte 2 (unassigned): update needed", key_alg,
               "update_needed", "unknown_algorithm", 3),
          Case("signature_algorithm_zero", "signed with the signature's id byte 0 (invalid): malformed", sig_zero,
               "malformed", "malformed", 2),
          Case("ext_unsorted", "signed with an ext_b64 whose entries are not sorted: malformed", bad_area, "malformed",
               "malformed", 2),
          Case("changed_field", "signed with seq 8: the signature does not hold", changed, "forgery", "bad_signature",
               4),
          Case("wrong_key", "signed with another valid key, one that does not derive peer_id", wrong_key, "forgery",
               "bad_signature", 4),
      }));
  return root;
}

Object ReadVectors() {
  std::ifstream file(PP_DHT_RECORD_VECTORS_PATH, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  auto root = common::TryParseObject(text.str());
  EXPECT_TRUE(root) << PP_DHT_RECORD_VECTORS_PATH;
  return root.value_or(Object{});
}

std::string FieldJson(const Object& object, const std::string& key) {
  auto slot = object.fields().tryGet(key);
  return slot ? common::DumpJson(slot->get()) : std::string("<missing>");
}

Value WithoutSignature(const Value& row) {
  const Object* row_obj = common::asObject(row);
  if (!row_obj) {
    return row;
  }
  Object copy = *row_obj;
  if (const Object* record = copy.getObject("record")) {
    Object stripped = *record;
    stripped.erase("signature_b64");
    copy.set("record", common::ObjectValue(std::move(stripped)));
  }
  return common::ObjectValue(std::move(copy));
}

// Everything but the hedged signatures is what this build makes: the signer, the input, the signed bytes, the records.
TEST(DhtRecordVectorsTest, TheFileIsWhatThisBuildMakes) {
  if (const char* out = std::getenv("PP_WRITE_DHT_RECORD_VECTORS"); out && *out) {
    std::ofstream file(out, std::ios::binary);
    file << common::DumpJson(BuildVectors(), 2) << "\n";
    ASSERT_TRUE(file.good()) << out;
    GTEST_SKIP() << "wrote " << out;
  }
  const Object root = ReadVectors();
  const Object now = BuildVectors();
  for (const char* field : {"format", "domain", "version", "signer", "input", "signed_bytes"}) {
    ASSERT_TRUE(root.contains(field)) << field;
    EXPECT_EQ(FieldJson(root, field), FieldJson(now, field))
        << field << " changed: a change to the signed bytes needs a new record version";
  }
  const common::Array* records = root.getArray("records");
  const common::Array* records_now = now.getArray("records");
  ASSERT_NE(records, nullptr);
  ASSERT_NE(records_now, nullptr);
  ASSERT_EQ(records->elements.size(), records_now->elements.size());
  for (size_t i = 0; i < records->elements.size(); ++i) {
    EXPECT_EQ(common::DumpJson(WithoutSignature(records->elements[i])),
              common::DumpJson(WithoutSignature(records_now->elements[i])));
  }
}

TEST(DhtRecordVectorsTest, TheSignerIsTheSeededDeviceKey) {
  const MlDsaKeyPair keys = KeysFromSeed(kSignerSeedHex);
  EXPECT_EQ(Sha256Hex(keys.public_key), kSignerPublicKeySha256Hex) << "pp-cpp-crypto's seeded keygen moved";
  EXPECT_EQ(pp::amp::PeerIdFromIdentityKey(keys.public_key), kSignerPeerId);
}

TEST(DhtRecordVectorsTest, TheSignedBytesAreFrozen) {
  const Object root = ReadVectors();
  const common::Array* rows = root.getArray("signed_bytes");
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->elements.size(), 2u);
  for (const Value& item : rows->elements) {
    const Object* row = common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const std::string ext_b64 = row->getString("ext_b64").value_or("x");
    auto entries = ext_b64.empty() ? Roe<ByteVector>(ByteVector{}) : Base64Decode(ext_b64); // "" is the empty area
    ASSERT_TRUE(static_cast<bool>(entries)) << name;
    auto ext = ExtArea::DecodeEntries(*entries);
    ASSERT_TRUE(static_cast<bool>(ext)) << name;
    PeerRoutingRecord record = FixedInput();
    record.ext = *ext;
    const ByteVector bytes = BuildPeerRoutingSignBytes(record);
    EXPECT_EQ(BytesToHex(bytes), row->getString("signed_bytes_hex").value_or("")) << name;
    // The header (§ 1.4): the domain without -vN, 00, the version byte; the area last.
    const std::string domain(kPeerRoutingRecordDomain);
    ASSERT_GT(bytes.size(), domain.size() + 2);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(domain.size())), domain);
    EXPECT_EQ(bytes[domain.size()], 0x00);
    EXPECT_EQ(bytes[domain.size() + 1], kPeerRoutingRecordVersion);
    const ByteVector area = ext->Encode();
    ASSERT_GE(bytes.size(), area.size());
    EXPECT_TRUE(std::equal(area.begin(), area.end(), bytes.end() - static_cast<std::ptrdiff_t>(area.size()))) << name;
  }
}

// Every record through the receiver's order: the frozen result, refusal and step.
TEST(DhtRecordVectorsTest, EveryRecordReadsAsFrozen) {
  const Object root = ReadVectors();
  const common::Array* rows = root.getArray("records");
  const common::Array* signed_bytes = root.getArray("signed_bytes");
  ASSERT_NE(rows, nullptr);
  ASSERT_NE(signed_bytes, nullptr);
  ASSERT_EQ(rows->elements.size(), 12u);
  const MlDsaKeyPair keys = KeysFromSeed(kSignerSeedHex);
  for (const Value& item : rows->elements) {
    const Object* row = common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const Object* record = row->getObject("record");
    ASSERT_NE(record, nullptr) << name;
    const std::string result = row->getString("result").value_or("");
    auto read = ReadPeerRoutingRecord(*record);
    if (result == "ok") {
      ASSERT_TRUE(static_cast<bool>(read)) << name << ": " << read.error().message;
      // Kept as received: the record (its extension entries too) writes back to the same JSON.
      EXPECT_EQ(common::DumpJson(PeerRoutingRecordToObject(*read)), common::DumpJson(*record)) << name;
      EXPECT_EQ(read->public_key_b64, PeerRoutingPublicKeyB64(keys.public_key).valueOr("")) << name;
      EXPECT_TRUE(static_cast<bool>(VerifyPeerRoutingRecord(*read, keys.public_key))) << name;
      // Its signed bytes are one of the frozen rows.
      const std::string hex = BytesToHex(BuildPeerRoutingSignBytes(*read));
      bool frozen = false;
      for (const Value& bytes_row : signed_bytes->elements) {
        frozen = frozen || common::asObject(bytes_row)->getString("signed_bytes_hex").value_or("") == hex;
      }
      EXPECT_TRUE(frozen) << name;
      continue;
    }
    ASSERT_FALSE(static_cast<bool>(read)) << name;
    const PeerRoutingRecordErr err = read.error().GetCode();
    EXPECT_EQ(ResultOf(err), result) << name << ": " << read.error().message;
    EXPECT_EQ(std::string(PeerRoutingRecordErrName(err)), row->getString("refusal").value_or("")) << name;
    EXPECT_EQ(StepOf(err), row->getIf<int64_t>("step").value_or(-1)) << name;
    EXPECT_EQ(PeerRoutingRecordNeedsUpdate(err), result == "update_needed") << name;
  }
}

// The orders that matter: a newer version is refused before the signature (it would not verify: its inside version
// is 2), an unknown critical entry only after it, and unknown non-critical entries are kept.
TEST(DhtRecordVectorsTest, TheRefusalsComeInTheReceiversOrder) {
  const Object root = ReadVectors();
  const common::Array* rows = root.getArray("records");
  ASSERT_NE(rows, nullptr);
  const auto record_of = [rows](const std::string& name) {
    for (const Value& item : rows->elements) {
      const Object* row = common::asObject(item);
      if (row && row->getString("name").value_or("") == name && row->getObject("record")) {
        return *row->getObject("record");
      }
    }
    ADD_FAILURE() << name;
    return Object{};
  };

  Object newer = record_of("version_plus_one");
  EXPECT_EQ(PeerRoutingRecordSupportOf(newer), PeerRoutingRecordSupport::UnsupportedVersion);
  newer.set("version", int64_t{kPeerRoutingRecordVersion});
  EXPECT_TRUE(static_cast<bool>(ReadPeerRoutingRecord(newer))) << "the same record at version 2 verifies";

  EXPECT_EQ(PeerRoutingRecordSupportOf(record_of("version_minus_one")), PeerRoutingRecordSupport::OlderVersion);
  EXPECT_EQ(PeerRoutingRecordSupportOf(record_of("version_missing")), PeerRoutingRecordSupport::OlderVersion);

  auto critical = PeerRoutingRecordFromObject(record_of("unknown_critical"));
  ASSERT_TRUE(static_cast<bool>(critical));
  EXPECT_TRUE(static_cast<bool>(VerifyPeerRoutingRecordStandalone(*critical))) << "it verifies first";
  EXPECT_TRUE(PeerRoutingRecordHasUnknownCritical(*critical));

  const Object with_entry = record_of("signed_one_non_critical");
  auto kept = ReadPeerRoutingRecord(with_entry);
  ASSERT_TRUE(static_cast<bool>(kept));
  ASSERT_EQ(kept->ext.entries().size(), 1u);
  EXPECT_EQ(kept->ext.entries()[0].type, 0x0001);
  EXPECT_FALSE(PeerRoutingRecordHasUnknownCritical(*kept));
  Object dropped = with_entry; // an entry dropped on the way changes the signed bytes
  dropped.erase("ext_b64");
  auto read_dropped = ReadPeerRoutingRecord(dropped);
  ASSERT_FALSE(static_cast<bool>(read_dropped));
  EXPECT_EQ(read_dropped.error().GetCode(), PeerRoutingRecordErr::BadSignature);
}

} // namespace
} // namespace pp::mesh
