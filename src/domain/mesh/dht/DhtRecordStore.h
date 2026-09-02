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
  mutable std::mutex mutex_;
  std::unordered_map<std::string, PeerRoutingRecord> by_peer_id_;
};

} // namespace pbr
