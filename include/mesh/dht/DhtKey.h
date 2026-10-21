#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pp::mesh {

/**
 * A point in the DHT's 256-bit key space (docs/DHT.md § 4.1). A node's ID and its routing record's
 * key are the same: `Key256(PeerId)`.
 */
using DhtKey = std::array<uint8_t, 32>;

/** Base58btc (Bitcoin alphabet) → bytes; nullopt on any character outside the alphabet. */
std::optional<std::vector<uint8_t>> Base58Decode(std::string_view text);

/**
 * `Key256(PeerId) = BLAKE2b-256("pp-mesh/dht/peer-routing/v1\0" ‖ PeerId multihash bytes)`
 * (pp-browser docs/contracts/MESH_DHT.md). Nullopt when `peer_id` is not base58.
 */
std::optional<DhtKey> DhtKeyOfPeerId(std::string_view peer_id);

/** XOR distance. */
DhtKey DhtDistance(const DhtKey& a, const DhtKey& b);

/**
 * The bucket `b` falls in as seen from `self`: 255 − (leading zero bits of the distance), so bucket
 * i holds distances in [2^i, 2^(i+1)). −1 when the keys are equal.
 */
int DhtBucketIndex(const DhtKey& self, const DhtKey& b);

/** `a` is strictly closer to `target` than `b` (XOR, big-endian). */
bool DhtCloser(const DhtKey& target, const DhtKey& a, const DhtKey& b);

} // namespace pp::mesh
