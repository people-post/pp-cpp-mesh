#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
 * one waits. A fixed wait after every tick capped what a node could take in — the kernel dropped the rest of a busy
 * socket's datagrams while the pump slept (pp-browser stress lab, 2026-11-06). An idle pump waits until the tick's
 * next deadline (Turn::next_due — timers, resends, the walks; at most `max_wait`), a datagram (SetIdleWait: the
 * socket's readiness) or a Wake (work another thread put on the lane). It waited 5 ms at a time before: an idle
 * phone woke 200 times a second for nothing (pp-browser #596).
 *
 * It never spins: a wait that ended early followed by a tick that neither read nor worked was woken for nothing;
 * after kSpuriousLimit of those in a row the pump waits plainly (no socket poll, Wake still ends it; at least
 * kGuardMinWait) for kGuardSpan, then polls again. Before, a socket that polled "ready" for ever with nothing to read
 * held a phone's core at 100 % (#596).
 */
class MeshPumpThread {
public:
  /** The legacy fixed wait (a tick without a deadline: Start's bool overload). */
  static constexpr std::chrono::milliseconds kDefaultInterval{5};

  /** What one tick did, and how soon the next is due. */
  struct Turn {
    /** Datagrams read. */
    size_t received = 0;
    /** It ran work besides reading (posted work, timers, a change another thread made on the lane). */
    bool worked = false;
    /** Its drain used the whole budget: datagrams are still waiting. */
    bool more = false;
    /** How soon the next tick has something to do (0: at once). */
    std::chrono::milliseconds next_due{kDefaultInterval};
  };

  /** The longest an idle pump waits when nothing is due sooner. */
  static constexpr std::chrono::milliseconds kMaxIdleWait{1000};
  /** The wait after a tick that left datagrams waiting: long enough for a thread blocked on the runtime's lock. */
  static constexpr std::chrono::microseconds kBusyBreather{200};
  /** Early waits followed by an empty tick, in a row, before the pump stops polling for a while. */
  static constexpr int kSpuriousLimit = 2;
  /** How long it then waits plainly, and the least each such wait lasts (the old fixed cadence). */
  static constexpr std::chrono::milliseconds kGuardSpan{1000};
  static constexpr std::chrono::milliseconds kGuardMinWait{5};

  MeshPumpThread() = default;
  ~MeshPumpThread();

  MeshPumpThread(const MeshPumpThread&) = delete;
  MeshPumpThread& operator=(const MeshPumpThread&) = delete;

  /**
   * Start the pump. `tick` (and the hooks, run on the pump thread before its first tick and after its last) must
   * remain valid until Stop() returns. An idle wait lasts until the tick's next_due, at most `max_wait`.
   */
  void Start(std::function<Turn()> tick, std::chrono::milliseconds max_wait = kMaxIdleWait,
             std::function<void()> on_thread_start = {}, std::function<void()> on_thread_exit = {});
  /** A tick without deadlines (true: more waiting): idle, it waits `interval` each time. */
  void Start(std::function<bool()> tick, std::chrono::milliseconds interval = kDefaultInterval,
             std::function<void()> on_thread_start = {}, std::function<void()> on_thread_exit = {});
  /** Signal stop and join. Idempotent. */
  void Stop();

  /**
   * Before Start: how an idle pump waits (`wait(most)`, on the pump thread, without the runtime's lock — e.g. the
   * endpoint's socket poll; true: the socket is readable) and how to end that wait from another thread (`wake`: Stop
   * and Wake call it). Unset: a timed wait.
   */
  void SetIdleWait(std::function<bool(std::chrono::milliseconds)> wait, std::function<void()> wake);

  /** Any thread: end the current idle wait now (or the next one, if the pump is ticking). */
  void Wake();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }
  /**
   * The share of the last full second the pump spent in ticks (0..1), not waiting: 1 is a pump that never catches
   * up. Any thread.
   */
  double BusyFraction() const { return busy_fraction_.load(std::memory_order_relaxed); }
  /** Times the pump stopped polling because its waits kept ending for nothing (see the class comment). Any thread. */
  uint64_t SpinGuards() const { return spin_guards_.load(std::memory_order_relaxed); }

private:
  void ThreadMain();
  /** Waits after `turn`: the idle wait or a plain one, per the spin guard. False once stopped. */
  bool WaitAfter(const Turn& turn);
  /** The condition variable's wait (Stop and Wake end it). False once stopped. */
  bool PlainWait(std::chrono::steady_clock::duration wait);

  /** Adds `busy` to this second's tally; at a second's end publishes its fraction. */
  void NoteBusy(std::chrono::steady_clock::duration busy, std::chrono::steady_clock::time_point now);
  /** kBusyBreather of yielding (not a timed wait: Windows rounds that up to a timer tick); false once stopped. */
  bool YieldBreather();

  std::function<Turn()> tick_;
  std::function<void()> on_thread_start_;
  std::function<void()> on_thread_exit_;
  std::function<bool(std::chrono::milliseconds)> idle_wait_;
  std::function<void()> idle_wake_;
  std::chrono::milliseconds max_wait_{kMaxIdleWait};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};
  bool stop_requested_ = false;
  /** Wake was called since the last tick began (mutex_). */
  bool wake_requested_ = false;
  std::atomic<double> busy_fraction_{0.0};
  std::chrono::steady_clock::time_point window_start_{};
  std::chrono::steady_clock::duration window_busy_{};
  /** The last wait ended well before it had to (pump thread). */
  bool last_wait_early_ = false;
  int spurious_in_a_row_ = 0;
  /** Until then the pump waits plainly (pump thread). */
  std::chrono::steady_clock::time_point guard_until_{};
  std::atomic<uint64_t> spin_guards_{0};
};

} // namespace pp::mesh
