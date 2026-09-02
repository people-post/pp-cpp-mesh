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

private:
  using Clock = std::chrono::steady_clock;

  void PruneLocked(std::deque<Clock::time_point>& times, Clock::time_point now) const;

  mutable std::mutex mutex_;
  int max_ops_per_window_ = 60;
  int window_seconds_ = 60;
  std::unordered_map<std::string, std::deque<Clock::time_point>> by_peer_;
};

} // namespace pbr
