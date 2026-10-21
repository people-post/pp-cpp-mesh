#pragma once

#include "mesh/dht/DhtTypes.h"

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace pp::mesh {

std::vector<uint8_t> BuildPeerRoutingSignBytes(const PeerRoutingRecord& record);

common::Object PeerRoutingRecordToObject(const PeerRoutingRecord& record);
Roe<PeerRoutingRecord> PeerRoutingRecordFromObject(const common::Object& object);

Roe<PeerRoutingRecord> SignPeerRoutingRecord(PeerRoutingRecord record,
                                             std::span<const uint8_t> device_signing_secret);

Roe<bool> VerifyPeerRoutingRecord(const PeerRoutingRecord& record,
                                  std::span<const uint8_t> signing_public_key);

/**
 * A record checked on its own: it carries a public key, that key derives its PeerId, and its
 * signature holds. How a record answered by another node is accepted.
 */
Roe<void> VerifyPeerRoutingRecordStandalone(const PeerRoutingRecord& record);

bool PeerRoutingRecordExpired(const PeerRoutingRecord& record, int64_t now_seconds,
                              int64_t grace_seconds = 60);

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
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromDhtRecords(
    const std::vector<PeerRoutingRecord>& records);

} // namespace pp::mesh
