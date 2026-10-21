#pragma once

#include "mesh/dht/DhtKey.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp::mesh {

/** One routing-table entry: a DHT participant we have exchanged an authenticated RPC with. */
struct DhtContact {
  std::string peer_id;
  DhtKey key{};
  /** Where to dial it (an ADP multiaddr). */
  std::string multiaddr;
  int64_t last_heard_ms = 0;
};

/**
 * Kademlia routing table (docs/DHT.md § 4.2): 256 buckets of up to `k` contacts by XOR distance
 * from our own key, in memory.
 *
 * - **Admission** is the caller's: only a peer that answered an authenticated DHT RPC is `NoteHeard`.
 *   A `closer_peers` hint is never an entry by itself.
 * - **Full bucket:** the oldest entry stays when it is alive; the newcomer waits in a small
 *   replacement list and the caller pings the oldest (`HeardResult::ping`). `Remove` (the record
 *   holds it down, or the ping went unanswered) lets the most recent replacement in.
 * - **Diversity:** at most `kMaxPerSubnet` entries per IPv4 /24 (IPv6 /48) per bucket.
 *
 * Not thread-safe: owned and driven on the mesh's io lane.
 */
class DhtRoutingTable {
public:
  static constexpr size_t kDefaultK = 20;
  static constexpr size_t kMaxPerSubnet = 2;
  static constexpr size_t kReplacements = 8;

  enum class Heard {
    /** New in its bucket. */
    Added,
    /** Already there: its address and time are refreshed. */
    Updated,
    /** Bucket full: waiting as a replacement; ping `HeardResult::ping` (the bucket's oldest). */
    Pending,
    /** Not admitted: ourselves, not a PeerId, or its subnet already fills its share of the bucket. */
    Rejected,
  };
  struct HeardResult {
    Heard heard = Heard::Rejected;
    std::optional<DhtContact> ping;
  };

  DhtRoutingTable(const std::string& self_peer_id, size_t k = kDefaultK);

  bool Valid() const { return self_key_.has_value(); }
  const DhtKey& SelfKey() const { return *self_key_; }

  /** `peer_id` answered an authenticated DHT RPC (or sent one): admit or refresh it. */
  HeardResult NoteHeard(const std::string& peer_id, const std::string& multiaddr, int64_t now_ms);
  /** Drop `peer_id` (gone, or a ping went unanswered); the bucket's newest replacement moves in. */
  void Remove(const std::string& peer_id);

  /** Up to `n` contacts closest to `target`, closest first. */
  std::vector<DhtContact> Closest(const DhtKey& target, size_t n) const;
  /** Buckets with contacts, none heard from since `older_than_ms` (refresh candidates). */
  std::vector<int> StaleBuckets(int64_t older_than_ms) const;
  size_t Size() const;
  bool Contains(const std::string& peer_id) const;

private:
  struct Bucket {
    std::vector<DhtContact> contacts;      // oldest heard first
    std::vector<DhtContact> replacements;  // newest last
  };

  static std::string SubnetOf(const std::string& multiaddr);

  std::optional<DhtKey> self_key_;
  std::string self_peer_id_;
  size_t k_;
  std::array<Bucket, 256> buckets_;
};

} // namespace pp::mesh
