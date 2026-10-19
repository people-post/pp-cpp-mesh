#include "domain/mesh/host/MeshPumpThread.h"

#include <utility>

namespace pbr {

MeshPumpThread::~MeshPumpThread() { Stop(); }

void MeshPumpThread::Start(std::function<void()> tick, const std::chrono::milliseconds interval,
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
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stop_requested_) {
        break;
      }
    }
    if (tick_) {
      tick_();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, interval_, [this]() { return stop_requested_; })) {
      break;
    }
  }
  if (on_thread_exit_) {
    on_thread_exit_();
  }
  running_.store(false, std::memory_order_release);
}

} // namespace pbr
