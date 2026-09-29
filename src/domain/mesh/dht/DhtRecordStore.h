#pragma once

#include "domain/mesh/dht/DhtTypes.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

/** Thread-safe peer_routing record cache (best seq wins per peer_id). */
class DhtRecordStore {
public:
  bool Put(PeerRoutingRecord record);
  std::optional<PeerRoutingRecord> Get(const std::string& peer_id) const;
  std::vector<PeerRoutingRecord> Snapshot() const;
  size_t Size() const;
  void Remove(const std::string& peer_id);

private:
  /** The record as received (its signed ttl_seconds is never rewritten) plus the store's own
   * clamped expiry, tracked separately so a re-verify of the stored record's signature still
   * matches what was signed. */
  struct StoredRecord {
    PeerRoutingRecord record;
    int64_t effective_expiry_seconds = 0;
  };

  void PruneExpiredLocked(int64_t now);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, StoredRecord> by_peer_id_;
};

} // namespace pbr
