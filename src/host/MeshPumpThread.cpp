#include "mesh/host/MeshPumpThread.h"

#include <algorithm>
#include <utility>

namespace pp::mesh {

MeshPumpThread::~MeshPumpThread() { Stop(); }

void MeshPumpThread::Start(std::function<bool()> tick, const std::chrono::milliseconds interval,
                           std::function<void()> on_thread_start, std::function<void()> on_thread_exit) {
  Stop();
  if (!tick) {
    return;
  }
  tick_ = std::move(tick);
  on_thread_start_ = std::move(on_thread_start);
  on_thread_exit_ = std::move(on_thread_exit);
  interval_ = interval.count() > 0 ? interval : kDefaultInterval;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = false;
  }
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { ThreadMain(); });
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
      std::unique_lock<std::mutex> lock(mutex_);
      if (stop_requested_) {
        break;
      }
    }
    const auto began = std::chrono::steady_clock::now();
    const bool more = tick_ && tick_();
    const auto ended = std::chrono::steady_clock::now();
    NoteBusy(ended - began, ended);
    // Datagrams still waiting: drain them almost at once — but not at once. A drive holds the runtime's lock; a pump
    // that took it straight back starved every other thread that needs it (status, posted work).
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, more ? kBusyBreather : interval_, [this]() { return stop_requested_; })) {
      break;
    }
  }
  if (on_thread_exit_) {
    on_thread_exit_();
  }
  running_.store(false, std::memory_order_release);
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
