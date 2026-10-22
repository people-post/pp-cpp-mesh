#include "mesh/dht/DhtRoutingTable.h"

#include <algorithm>

namespace pp::mesh {

DhtRoutingTable::DhtRoutingTable(const size_t k) : k_(k > 0 ? k : kDefaultK) {}

DhtRoutingTable::DhtRoutingTable(const std::string& self_peer_id, const size_t k) : DhtRoutingTable(k) {
  Reset(self_peer_id);
}

void DhtRoutingTable::Reset(const std::string& self_peer_id, const size_t k) {
  std::lock_guard lock(mutex_);
  const size_t new_k = k > 0 ? k : k_;
  if (self_key_ && self_peer_id == self_peer_id_ && new_k == k_) {
    return;
  }
  k_ = new_k;
  self_key_ = self_peer_id.empty() ? std::nullopt : DhtKeyOfPeerId(self_peer_id);
  self_peer_id_ = self_peer_id;
  for (Bucket& bucket : buckets_) {
    bucket = Bucket{};
  }
}

size_t DhtRoutingTable::K() const {
  std::lock_guard lock(mutex_);
  return k_;
}

bool DhtRoutingTable::Valid() const {
  std::lock_guard lock(mutex_);
  return self_key_.has_value();
}

DhtKey DhtRoutingTable::SelfKey() const {
  std::lock_guard lock(mutex_);
  return self_key_.value_or(DhtKey{});
}

DhtRoutingTable::Bucket* DhtRoutingTable::BucketOfLocked(const std::string& peer_id,
                                                         std::optional<DhtKey>* key_out) {
  if (!self_key_ || peer_id.empty() || peer_id == self_peer_id_) {
    return nullptr;
  }
  const auto key = DhtKeyOfPeerId(peer_id);
  const int index = key ? DhtBucketIndex(*self_key_, *key) : -1;
  if (index < 0) {
    return nullptr;
  }
  if (key_out) {
    *key_out = key;
  }
  return &buckets_[static_cast<size_t>(index)];
}

std::string DhtRoutingTable::SubnetOf(const std::string& multiaddr) {
  // "/ip4/a.b.c.d/..." → "4:a.b.c"; "/ip6/x:x:x:..." → "6:x:x:x" (/48); else "" (no cap).
  auto host_of = [&](const std::string& prefix) -> std::string {
    if (multiaddr.rfind(prefix, 0) != 0) {
      return {};
    }
    const size_t start = prefix.size();
    const size_t end = multiaddr.find('/', start);
    return multiaddr.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };
  if (const std::string v4 = host_of("/ip4/"); !v4.empty()) {
    const size_t last_dot = v4.rfind('.');
    return last_dot == std::string::npos ? std::string() : "4:" + v4.substr(0, last_dot);
  }
  if (const std::string v6 = host_of("/ip6/"); !v6.empty()) {
    std::string out = "6:";
    int colons = 0;
    for (const char c : v6) {
      if (c == ':' && ++colons == 3) {
        break;
      }
      out += c;
    }
    return out;
  }
  return {};
}

DhtRoutingTable::HeardResult DhtRoutingTable::NoteHeard(const std::string& peer_id, const std::string& multiaddr,
                                                    const int64_t now_ms) {
  HeardResult result;
  std::lock_guard lock(mutex_);
  std::optional<DhtKey> key;
  Bucket* found = BucketOfLocked(peer_id, &key);
  if (!found) {
    return result;
  }
  Bucket& bucket = *found;
  auto same = [&](const DhtContact& c) { return c.peer_id == peer_id; };
  if (auto it = std::find_if(bucket.contacts.begin(), bucket.contacts.end(), same); it != bucket.contacts.end()) {
    DhtContact contact = *it;
    contact.last_heard_ms = now_ms;
    if (!multiaddr.empty()) {
      contact.multiaddr = multiaddr;
    }
    bucket.contacts.erase(it);
    bucket.contacts.push_back(std::move(contact));  // most recently heard last
    result.heard = Heard::Updated;
    return result;
  }
  const std::string subnet = SubnetOf(multiaddr);
  if (!subnet.empty()) {
    const auto same_subnet = std::count_if(bucket.contacts.begin(), bucket.contacts.end(),
                                           [&](const DhtContact& c) { return SubnetOf(c.multiaddr) == subnet; });
    if (static_cast<size_t>(same_subnet) >= kMaxPerSubnet) {
      return result;  // Rejected: that subnet holds its share of this bucket
    }
  }
  DhtContact contact{peer_id, *key, multiaddr, now_ms};
  if (bucket.contacts.size() < k_) {
    bucket.contacts.push_back(std::move(contact));
    result.heard = Heard::Added;
    return result;
  }
  // Full: keep the long-lived entries; the newcomer waits, the oldest gets pinged.
  std::erase_if(bucket.replacements, same);
  bucket.replacements.push_back(std::move(contact));
  if (bucket.replacements.size() > kReplacements) {
    bucket.replacements.erase(bucket.replacements.begin());
  }
  result.heard = Heard::Pending;
  result.ping = bucket.contacts.front();
  return result;
}

void DhtRoutingTable::Remove(const std::string& peer_id) {
  std::lock_guard lock(mutex_);
  Bucket* found = BucketOfLocked(peer_id);
  if (!found) {
    return;
  }
  Bucket& bucket = *found;
  auto same = [&](const DhtContact& c) { return c.peer_id == peer_id; };
  const auto before = bucket.contacts.size();
  std::erase_if(bucket.contacts, same);
  std::erase_if(bucket.replacements, same);
  if (bucket.contacts.size() < before && !bucket.replacements.empty() && bucket.contacts.size() < k_) {
    bucket.contacts.push_back(std::move(bucket.replacements.back()));
    bucket.replacements.pop_back();
  }
}

std::vector<DhtContact> DhtRoutingTable::Closest(const DhtKey& target, const size_t n,
                                                 const std::string& exclude_peer_id) const {
  std::vector<DhtContact> all;
  {
    std::lock_guard lock(mutex_);
    for (const Bucket& bucket : buckets_) {
      for (const DhtContact& c : bucket.contacts) {
        if (c.peer_id != exclude_peer_id) {
          all.push_back(c);
        }
      }
    }
  }
  const size_t take = std::min(n, all.size());
  std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(take), all.end(),
                    [&](const DhtContact& a, const DhtContact& b) { return DhtCloser(target, a.key, b.key); });
  all.resize(take);
  return all;
}

std::vector<DhtContact> DhtRoutingTable::InBucket(const int index) const {
  std::lock_guard lock(mutex_);
  if (index < 0 || static_cast<size_t>(index) >= buckets_.size()) {
    return {};
  }
  return buckets_[static_cast<size_t>(index)].contacts;
}

std::vector<int> DhtRoutingTable::StaleBuckets(const int64_t older_than_ms) const {
  std::lock_guard lock(mutex_);
  std::vector<int> stale;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    const auto& contacts = buckets_[i].contacts;
    if (!contacts.empty() &&
        std::all_of(contacts.begin(), contacts.end(), [&](const DhtContact& c) { return c.last_heard_ms < older_than_ms; })) {
      stale.push_back(static_cast<int>(i));
    }
  }
  return stale;
}

size_t DhtRoutingTable::Size() const {
  std::lock_guard lock(mutex_);
  size_t n = 0;
  for (const Bucket& bucket : buckets_) {
    n += bucket.contacts.size();
  }
  return n;
}

bool DhtRoutingTable::Contains(const std::string& peer_id) const { return Find(peer_id).has_value(); }

std::optional<DhtContact> DhtRoutingTable::Find(const std::string& peer_id) const {
  std::lock_guard lock(mutex_);
  for (const Bucket& bucket : buckets_) {
    for (const DhtContact& c : bucket.contacts) {
      if (c.peer_id == peer_id) {
        return c;
      }
    }
  }
  return std::nullopt;
}

} // namespace pp::mesh
