#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace pbr {

/**
 * Joinable Amp UDP drain thread. Owns the ~5ms MeshHost::Tick / MeshRuntime::Drive loop.
 * Owned by MeshHost — coordinator must not run this work.
 */
class MeshPumpThread {
public:
  static constexpr std::chrono::milliseconds kDefaultInterval{5};

  MeshPumpThread() = default;
  ~MeshPumpThread();

  MeshPumpThread(const MeshPumpThread&) = delete;
  MeshPumpThread& operator=(const MeshPumpThread&) = delete;

  /** Start the pump. `tick` must remain valid until Stop() returns. */
  void Start(std::function<void()> tick, std::chrono::milliseconds interval = kDefaultInterval);
  /** Signal stop and join. Idempotent. */
  void Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
  void ThreadMain();

  std::function<void()> tick_;
  std::chrono::milliseconds interval_{kDefaultInterval};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};
  bool stop_requested_ = false;
};

} // namespace pbr
