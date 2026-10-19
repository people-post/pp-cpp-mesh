#include "domain/mesh/dht/DhtRecordStore.h"

#include "domain/mesh/dht/DhtRecordCodec.h"

#include <ctime>
#include <limits>

namespace pbr {

namespace {
/** Peer-controlled ttl (see AmpDhtProtocol inbound "store") — cap so a record can't outlive this. */
constexpr int64_t kMaxRecordTtlSeconds = 24 * 3600;
/** Bound total memory: one entry per distinct peer_id (unbounded otherwise — a DoS vector). */
constexpr size_t kMaxRecords = 4096;

/** issued_at + min(ttl_seconds, kMaxRecordTtlSeconds), without touching the record itself. */
int64_t EffectiveExpirySeconds(const PeerRoutingRecord& record) {
  const int64_t capped_ttl = std::min(record.ttl_seconds, kMaxRecordTtlSeconds);
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  if (record.issued_at > kMax - capped_ttl) {
    return kMax;
  }
  return record.issued_at + capped_ttl;
}

bool StoredExpired(const int64_t effective_expiry_seconds, const int64_t now_seconds) {
  return now_seconds > effective_expiry_seconds;
}
} // namespace

bool DhtRecordStore::Put(PeerRoutingRecord record) {
  if (record.peer_id.empty()) {
    return false;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (PeerRoutingRecordExpired(record, now)) {
    return false;
  }
  const int64_t effective_expiry = EffectiveExpirySeconds(record);
  if (StoredExpired(effective_expiry, now)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto it = by_peer_id_.find(record.peer_id);
  if (it != by_peer_id_.end() && it->second.record.seq > record.seq) {
    return false;
  }
  if (it == by_peer_id_.end() && by_peer_id_.size() >= kMaxRecords) {
    PruneExpiredLocked(now);
    if (by_peer_id_.size() >= kMaxRecords) {
      return false; // Full of still-live records — refuse rather than evict a good one.
    }
  }
  // record.peer_id must be read before record is moved-from: in `map[k] = v`, the assignment's
  // right operand (v) is sequenced before the left operand (map[k]) since C++17, so passing
  // record.peer_id directly to operator[] here would read it after std::move(record) already
  // cleared it, inserting under an empty key.
  const std::string peer_id = record.peer_id;
  by_peer_id_[peer_id] = StoredRecord{std::move(record), effective_expiry};
  return true;
}

/** Caller holds mutex_. */
void DhtRecordStore::PruneExpiredLocked(const int64_t now) {
  for (auto it = by_peer_id_.begin(); it != by_peer_id_.end();) {
    if (StoredExpired(it->second.effective_expiry_seconds, now)) {
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
  for (const auto& [peer_id, stored] : by_peer_id_) {
    (void)peer_id;
    if (!StoredExpired(stored.effective_expiry_seconds, now)) {
      out.push_back(stored.record);
    }
  }
  return out;
}

size_t DhtRecordStore::Size() const {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [peer_id, stored] : by_peer_id_) {
    (void)peer_id;
    if (!StoredExpired(stored.effective_expiry_seconds, now)) {
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
  if (StoredExpired(it->second.effective_expiry_seconds, now)) {
    return std::nullopt;
  }
  return it->second.record;
}

void DhtRecordStore::Remove(const std::string& peer_id) {
  std::lock_guard lock(mutex_);
  by_peer_id_.erase(peer_id);
}

} // namespace pbr
