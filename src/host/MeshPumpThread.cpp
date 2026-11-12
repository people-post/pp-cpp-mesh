#include "mesh/host/MeshPumpThread.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace pp::mesh {

MeshPumpThread::~MeshPumpThread() {
  Stop();
}

void MeshPumpThread::Start(std::function<Turn()> tick, const std::chrono::milliseconds max_wait,
                           std::function<void()> on_thread_start, std::function<void()> on_thread_exit) {
  Stop();
  if (!tick) {
    return;
  }
  tick_ = std::move(tick);
  on_thread_start_ = std::move(on_thread_start);
  on_thread_exit_ = std::move(on_thread_exit);
  max_wait_ = max_wait.count() > 0 ? max_wait : kMaxIdleWait;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = false;
    wake_requested_ = false;
  }
  last_wait_early_ = false;
  spurious_in_a_row_ = 0;
  guard_until_ = {};
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { ThreadMain(); });
}

void MeshPumpThread::Start(std::function<bool()> tick, const std::chrono::milliseconds interval,
                           std::function<void()> on_thread_start, std::function<void()> on_thread_exit) {
  if (!tick) {
    Stop();
    return;
  }
  const auto every = interval.count() > 0 ? interval : kDefaultInterval;
  // No deadlines: the old fixed wait, and never taken for a spin (it says nothing about work).
  Start(
      [tick = std::move(tick), every]() {
        Turn turn;
        turn.more = tick();
        turn.worked = true;
        turn.next_due = every;
        return turn;
      },
      every, std::move(on_thread_start), std::move(on_thread_exit));
}

void MeshPumpThread::SetIdleWait(std::function<bool(std::chrono::milliseconds)> wait, std::function<void()> wake) {
  idle_wait_ = std::move(wait);
  idle_wake_ = std::move(wake);
}

void MeshPumpThread::Wake() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_requested_ = true;
  }
  cv_.notify_all();
  if (idle_wake_) {
    idle_wake_();
  }
}

void MeshPumpThread::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load(std::memory_order_acquire) && !thread_.joinable()) {
      return;
    }
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (idle_wake_) {
    idle_wake_();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false, std::memory_order_release);
  tick_ = nullptr;
  on_thread_start_ = nullptr;
  on_thread_exit_ = nullptr;
}

void MeshPumpThread::ThreadMain() {
  if (on_thread_start_) {
    on_thread_start_();
  }
  window_start_ = std::chrono::steady_clock::now();
  window_busy_ = {};
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_requested_) {
        break;
      }
      wake_requested_ = false; // this tick sees what the wake was for
    }
    const auto began = std::chrono::steady_clock::now();
    const Turn turn = tick_ ? tick_() : Turn{};
    const auto ended = std::chrono::steady_clock::now();
    NoteBusy(ended - began, ended);
    if (turn.received > 0 || turn.worked || turn.more) {
      spurious_in_a_row_ = 0;
      if (turn.received > 0) {
        guard_until_ = {}; // the socket reads: poll it again
      }
    } else if (last_wait_early_ && ++spurious_in_a_row_ >= kSpuriousLimit) {
      // Woken again and again for nothing (a socket or wake handle "ready" for ever): stop polling for a while.
      spurious_in_a_row_ = 0;
      guard_until_ = ended + kGuardSpan;
      spin_guards_.fetch_add(1, std::memory_order_relaxed);
    }
    if (turn.more) {
      // Datagrams still waiting: drain them almost at once — but not at once. A drive holds the runtime's lock; a
      // pump that took it straight back starved every other thread that needs it (status, posted work). The
      // breather yields rather than waits: a timed wait on Windows sleeps a whole timer tick (1-16 ms), longer than
      // the idle interval, and the busy pump there was no faster than an idle one (MeshPumpThreadTest on
      // windows-2022, v6.2.0 to v6.4.0).
      last_wait_early_ = false;
      if (!YieldBreather()) {
        break;
      }
      continue;
    }
    if (!WaitAfter(turn)) {
      break;
    }
  }
  if (on_thread_exit_) {
    on_thread_exit_();
  }
  running_.store(false, std::memory_order_release);
}

bool MeshPumpThread::WaitAfter(const Turn& turn) {
  const auto now = std::chrono::steady_clock::now();
  const bool guarded = now < guard_until_;
  auto wait = std::clamp(turn.next_due, std::chrono::milliseconds(0), max_wait_);
  if (guarded) {
    wait = std::max(wait, kGuardMinWait);
  }
  if (wait.count() == 0) {
    // Due at once (posted work over the drain's budget): the breather, then the next tick. A tick that keeps saying
    // "now" and does nothing counts as woken for nothing: the guard ends it.
    last_wait_early_ = true;
    return YieldBreather();
  }
  bool going = true;
  if (idle_wait_ && !guarded) {
    // Until a datagram arrives, the deadline passes, or Wake / Stop ends it.
    (void)idle_wait_(wait);
    std::lock_guard<std::mutex> lock(mutex_);
    going = !stop_requested_;
  } else {
    going = PlainWait(wait);
  }
  last_wait_early_ = std::chrono::steady_clock::now() - now + std::chrono::milliseconds(1) < wait;
  return going;
}

bool MeshPumpThread::PlainWait(const std::chrono::steady_clock::duration wait) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, wait, [this]() { return stop_requested_ || wake_requested_; });
  return !stop_requested_;
}

bool MeshPumpThread::YieldBreather() {
  const auto until = std::chrono::steady_clock::now() + kBusyBreather;
  do {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_requested_) {
        return false;
      }
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < until);
  return true;
}

void MeshPumpThread::NoteBusy(const std::chrono::steady_clock::duration busy,
                              const std::chrono::steady_clock::time_point now) {
  window_busy_ += busy;
  const auto window = now - window_start_;
  if (window >= std::chrono::seconds(1)) {
    busy_fraction_.store(std::min(1.0, std::chrono::duration<double>(window_busy_).count() /
                                           std::chrono::duration<double>(window).count()),
                         std::memory_order_relaxed);
    window_start_ = now;
    window_busy_ = {};
  }
}

} // namespace pp::mesh
