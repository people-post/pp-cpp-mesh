#include "domain/mesh/host/MeshPumpThread.h"

#include <utility>

namespace pbr {

MeshPumpThread::~MeshPumpThread() { Stop(); }

void MeshPumpThread::Start(std::function<void()> tick, const std::chrono::milliseconds interval) {
  Stop();
  if (!tick) {
    return;
  }
  tick_ = std::move(tick);
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
}

void MeshPumpThread::ThreadMain() {
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
  running_.store(false, std::memory_order_release);
}

} // namespace pbr
