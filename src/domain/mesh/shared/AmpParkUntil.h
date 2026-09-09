#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace pbr {

/**
 * Park until `done` or `deadline`.
 *
 * Product MeshPump drives Amp — pass an empty `io_pump` so waiters sleep instead of
 * contending on `MeshHost::Tick` / `Drive`. Harnesses without a pump supply `io_pump`
 * (typically `Tick` / `PumpBoth`) so callbacks still progress.
 */
inline void AmpParkUntil(const std::function<bool()>& done,
                         const std::chrono::steady_clock::time_point deadline,
                         const std::function<void()>& io_pump = {}) {
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    if (io_pump) {
      io_pump();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

/**
 * Wait until `is_open` (channel Opening→Open) without blocking MeshPump.
 * When `post_io` is set (MeshRuntime::PostToIo), poll via io queue. Otherwise AmpParkUntil.
 */
inline void AmpScheduleWhenChannelOpen(const std::function<void(std::function<void()>)>& post_io,
                                       const std::function<void()>& io_pump,
                                       const std::function<bool()>& is_open,
                                       const std::chrono::steady_clock::time_point deadline,
                                       std::function<void(bool open)> done,
                                       const std::function<bool()>& aborted = {}) {
  if (!done) {
    return;
  }
  if (!post_io) {
    if (aborted && aborted()) {
      done(false);
      return;
    }
    AmpParkUntil(
        [&] { return (aborted && aborted()) || is_open(); }, deadline, io_pump);
    if (aborted && aborted()) {
      done(false);
      return;
    }
    done(is_open());
    return;
  }

  auto attempt = std::make_shared<std::function<void()>>();
  *attempt = [post_io, is_open, deadline, done = std::move(done), aborted, attempt]() mutable {
    if (aborted && aborted()) {
      done(false);
      return;
    }
    if (is_open()) {
      done(true);
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      done(false);
      return;
    }
    post_io([attempt]() { (*attempt)(); });
  };
  post_io([attempt]() { (*attempt)(); });
}

} // namespace pbr
