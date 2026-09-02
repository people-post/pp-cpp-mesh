#include "base/mesh/dht/DhtRecordStore.h"

#include "base/mesh/dht/DhtRecordCodec.h"

#include <ctime>

namespace pbr {

bool DhtRecordStore::Put(PeerRoutingRecord record) {
  if (record.peer_id.empty()) {
    return false;
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
  by_peer_id_[record.peer_id] = std::move(record);
  return true;
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
