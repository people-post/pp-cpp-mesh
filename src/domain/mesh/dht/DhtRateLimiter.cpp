#include "domain/mesh/dht/DhtRateLimiter.h"

#include <iterator>

namespace pbr {

DhtRateLimiter::DhtRateLimiter(const int max_ops_per_window, const int window_seconds) {
  Configure(max_ops_per_window, window_seconds);
}

void DhtRateLimiter::Configure(const int max_ops_per_window, const int window_seconds) {
  std::lock_guard lock(mutex_);
  max_ops_per_window_ = max_ops_per_window > 0 ? max_ops_per_window : 60;
  window_seconds_ = window_seconds > 0 ? window_seconds : 60;
}

void DhtRateLimiter::PruneLocked(std::deque<Clock::time_point>& times, const Clock::time_point now) const {
  const auto cutoff = now - std::chrono::seconds(window_seconds_);
  while (!times.empty() && times.front() < cutoff) {
    times.pop_front();
  }
}

bool DhtRateLimiter::Allow(const std::string& peer_key) {
  const std::string key = peer_key.empty() ? "_" : peer_key;
  std::lock_guard lock(mutex_);
  const auto now = Clock::now();
  auto& times = by_peer_[key];
  PruneLocked(times, now);
  if (static_cast<int>(times.size()) >= max_ops_per_window_) {
    return false;
  }
  times.push_back(now);
  // Peers whose ops all left the window keep nothing; without this sweep every peer id ever
  // seen kept its (empty) entry.
  if (++grants_since_sweep_ >= kSweepEveryGrants) {
    grants_since_sweep_ = 0;
    for (auto it = by_peer_.begin(); it != by_peer_.end();) {
      PruneLocked(it->second, now);
      it = it->second.empty() ? by_peer_.erase(it) : std::next(it);
    }
  }
  return true;
}

size_t DhtRateLimiter::TrackedPeers() const {
  std::lock_guard lock(mutex_);
  return by_peer_.size();
}

void DhtRateLimiter::Clear() {
  std::lock_guard lock(mutex_);
  by_peer_.clear();
}

} // namespace pbr
