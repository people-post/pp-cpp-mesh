#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <deque>

namespace pbr {

/**
 * Per-peer sliding-window rate limit for DHT control ops (n2-hard).
 * Window is wall-clock seconds; counts completed Allow() grants.
 */
class DhtRateLimiter {
public:
  explicit DhtRateLimiter(int max_ops_per_window = 60, int window_seconds = 60);

  void Configure(int max_ops_per_window, int window_seconds);

  /** True when under limit; records the grant. Empty peer_key uses a shared bucket. */
  bool Allow(const std::string& peer_key);

  void Clear();

  /** Peers with an entry (tests: idle peers are dropped). */
  size_t TrackedPeers() const;

  /** Idle peers are dropped once per this many grants. */
  static constexpr size_t kSweepEveryGrants = 256;

private:
  using Clock = std::chrono::steady_clock;

  void PruneLocked(std::deque<Clock::time_point>& times, Clock::time_point now) const;

  mutable std::mutex mutex_;
  int max_ops_per_window_ = 60;
  int window_seconds_ = 60;
  std::unordered_map<std::string, std::deque<Clock::time_point>> by_peer_;
  /** Grants since peers with no op left in the window were last dropped. */
  size_t grants_since_sweep_ = 0;
};

} // namespace pbr
