#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace pp::mesh {

/**
 * Joinable Amp UDP drain thread. Owns the MeshHost::Tick / MeshRuntime::Drive loop — the runtime's only driver:
 * `on_thread_start` claims it (MeshRuntime::ClaimDriver), `on_thread_exit` releases it. Owned by MeshHost —
 * coordinator must not run this work.
 *
 * A tick that says more is waiting (its drain used the whole budget) is followed by the next at once; only an idle
 * one waits `interval`. A fixed wait after every tick capped what a node could take in — the kernel dropped the rest
 * of a busy socket's datagrams while the pump slept (pp-browser stress lab, 2026-11-06).
 */
class MeshPumpThread {
public:
  static constexpr std::chrono::milliseconds kDefaultInterval{5};
  /** The wait after a tick that left datagrams waiting: long enough for a thread blocked on the runtime's lock. */
  static constexpr std::chrono::microseconds kBusyBreather{200};

  MeshPumpThread() = default;
  ~MeshPumpThread();

  MeshPumpThread(const MeshPumpThread&) = delete;
  MeshPumpThread& operator=(const MeshPumpThread&) = delete;

  /**
   * Start the pump. `tick` (and the hooks, run on the pump thread before its first tick and after
   * its last) must remain valid until Stop() returns.
   */
  void Start(std::function<bool()> tick, std::chrono::milliseconds interval = kDefaultInterval,
             std::function<void()> on_thread_start = {}, std::function<void()> on_thread_exit = {});
  /** Signal stop and join. Idempotent. */
  void Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }
  /**
   * The share of the last full second the pump spent in ticks (0..1), not waiting: 1 is a pump that never catches
   * up. Any thread.
   */
  double BusyFraction() const { return busy_fraction_.load(std::memory_order_relaxed); }

private:
  void ThreadMain();

  /** Adds `busy` to this second's tally; at a second's end publishes its fraction. */
  void NoteBusy(std::chrono::steady_clock::duration busy, std::chrono::steady_clock::time_point now);

  std::function<bool()> tick_;
  std::function<void()> on_thread_start_;
  std::function<void()> on_thread_exit_;
  std::chrono::milliseconds interval_{kDefaultInterval};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};
  bool stop_requested_ = false;
  std::atomic<double> busy_fraction_{0.0};
  std::chrono::steady_clock::time_point window_start_{};
  std::chrono::steady_clock::duration window_busy_{};
};

} // namespace pp::mesh
