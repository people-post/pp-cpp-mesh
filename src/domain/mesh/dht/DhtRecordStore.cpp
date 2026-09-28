#include "domain/mesh/dht/DhtRecordStore.h"

#include "domain/mesh/dht/DhtRecordCodec.h"

#include <ctime>

namespace pbr {

namespace {
/** Peer-controlled ttl (see AmpDhtProtocol inbound "store") — cap so a record can't outlive this. */
constexpr int64_t kMaxRecordTtlSeconds = 24 * 3600;
/** Bound total memory: one entry per distinct peer_id (unbounded otherwise — a DoS vector). */
constexpr size_t kMaxRecords = 4096;
} // namespace

bool DhtRecordStore::Put(PeerRoutingRecord record) {
  if (record.peer_id.empty()) {
    return false;
  }
  if (record.ttl_seconds > kMaxRecordTtlSeconds) {
    record.ttl_seconds = kMaxRecordTtlSeconds;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (PeerRoutingRecordExpired(record, now)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto it = by_peer_id_.find(record.peer_id);
  if (it != by_peer_id_.end() && it->second.seq > record.seq) {
    return false;
  }
  if (it == by_peer_id_.end() && by_peer_id_.size() >= kMaxRecords) {
    PruneExpiredLocked(now);
    if (by_peer_id_.size() >= kMaxRecords) {
      return false; // Full of still-live records — refuse rather than evict a good one.
    }
  }
  by_peer_id_[record.peer_id] = std::move(record);
  return true;
}

/** Caller holds mutex_. */
void DhtRecordStore::PruneExpiredLocked(const int64_t now) {
  for (auto it = by_peer_id_.begin(); it != by_peer_id_.end();) {
    if (PeerRoutingRecordExpired(it->second, now)) {
      it = by_peer_id_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<PeerRoutingRecord> DhtRecordStore::Snapshot() const {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::lock_guard lock(mutex_);
  std::vector<PeerRoutingRecord> out;
  out.reserve(by_peer_id_.size());
  for (const auto& [peer_id, record] : by_peer_id_) {
    (void)peer_id;
    if (!PeerRoutingRecordExpired(record, now)) {
      out.push_back(record);
    }
  }
  return out;
}

size_t DhtRecordStore::Size() const {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [peer_id, record] : by_peer_id_) {
    (void)peer_id;
    if (!PeerRoutingRecordExpired(record, now)) {
      ++count;
    }
  }
  return count;
}

std::optional<PeerRoutingRecord> DhtRecordStore::Get(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const auto it = by_peer_id_.find(peer_id);
  if (it == by_peer_id_.end()) {
    return std::nullopt;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (PeerRoutingRecordExpired(it->second, now)) {
    return std::nullopt;
  }
  return it->second;
}

void DhtRecordStore::Remove(const std::string& peer_id) {
  std::lock_guard lock(mutex_);
  by_peer_id_.erase(peer_id);
}

} // namespace pbr
