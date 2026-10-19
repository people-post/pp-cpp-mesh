#pragma once

#include "domain/mesh/dht/DhtTypes.h"

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

std::vector<uint8_t> BuildPeerRoutingSignBytes(const PeerRoutingRecord& record);

Object PeerRoutingRecordToObject(const PeerRoutingRecord& record);
Roe<PeerRoutingRecord> PeerRoutingRecordFromObject(const Object& object);

Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record,
                                             std::span<const uint8_t> device_signing_secret);

Roe<bool> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                  std::span<const uint8_t> signing_public_key);

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, int64_t now_seconds,
                              int64_t grace_seconds = 60);

/** Map DHT record → directory-shaped node for hop policy (n2-caps). */
MeshDirectoryNode MeshDirectoryNodeFromDhtRecord(const PeerRoutingRecord& record);
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromDhtRecords(
    const std::vector<PeerRoutingRecord>& records);

} // namespace pbr
