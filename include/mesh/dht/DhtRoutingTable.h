#pragma once

#include "mesh/dht/DhtKey.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
 * Thread-safe: the server's workers and the client's callbacks share one table.
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

  /** No self yet (`Valid()` false) until `Reset`. */
  explicit DhtRoutingTable(size_t k = kDefaultK);
  DhtRoutingTable(const std::string& self_peer_id, size_t k = kDefaultK);

  DhtRoutingTable(const DhtRoutingTable&) = delete;
  DhtRoutingTable& operator=(const DhtRoutingTable&) = delete;

  /** Start over around `self_peer_id` (empty: no self); a no-op when it is already ours. */
  void Reset(const std::string& self_peer_id);
  bool Valid() const;
  /** Our own key (all zero before `Reset`). */
  DhtKey SelfKey() const;

  /** `peer_id` answered an authenticated DHT RPC (or sent one): admit or refresh it. */
  HeardResult NoteHeard(const std::string& peer_id, const std::string& multiaddr, int64_t now_ms);
  /** Drop `peer_id` (gone, or a ping went unanswered); the bucket's newest replacement moves in. */
  void Remove(const std::string& peer_id);

  /** Up to `n` contacts closest to `target`, closest first; never `exclude_peer_id`. */
  std::vector<DhtContact> Closest(const DhtKey& target, size_t n, const std::string& exclude_peer_id = {}) const;
  /** The contact for `peer_id`, if it is one. */
  std::optional<DhtContact> Find(const std::string& peer_id) const;
  /** The contacts in bucket `index` (0–255). */
  std::vector<DhtContact> InBucket(int index) const;
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
  Bucket* BucketOfLocked(const std::string& peer_id, std::optional<DhtKey>* key_out = nullptr);

  mutable std::mutex mutex_;
  std::optional<DhtKey> self_key_;
  std::string self_peer_id_;
  size_t k_;
  std::array<Bucket, 256> buckets_;
};

} // namespace pp::mesh
