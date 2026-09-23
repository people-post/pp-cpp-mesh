#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace pbr {

/** MeshRuntime::PostAfter — Amp-clock delayed work (deadlines / sync windows). */
using MeshIoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

/**
 * Park until `done` or `deadline`.
 *
 * Exclusive Amp Drive: pass an empty `io_pump` so waiters sleep while MeshPump (or the
 * harness Tick/Drive loop acting as Amp) progresses. Do not pass a callback that calls
 * Tick/Drive from L4 SM / PostToIo work — nested Drive is refused.
 * Harness sync facades may pass PumpAll only when the waiter thread *is* the sole Amp driver.
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

/**
 * Fire `on_timeout` once if still unsettled at `deadline`.
 * Prefer `io_after` (MeshRuntime::PostAfter) — one Amp-clock timer, no busy PostToIo poll.
 * Else with `post_io`, re-queues until deadline (legacy). Else AmpParkUntil (harness Tick).
 */
inline void AmpScheduleUntilSettled(const std::function<void(std::function<void()>)>& post_io,
                                    const std::function<void()>& io_pump,
                                    const std::shared_ptr<std::atomic<bool>>& settled,
                                    const std::chrono::steady_clock::time_point deadline,
                                    std::function<void()> on_timeout,
                                    const MeshIoAfter& io_after = {}) {
  if (!settled) {
    return;
  }
  if (io_after) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      if (!settled->load(std::memory_order_acquire) && on_timeout) {
        on_timeout();
      }
      return;
    }
    io_after(remaining, [settled, on_timeout = std::move(on_timeout)]() mutable {
      if (!settled->load(std::memory_order_acquire) && on_timeout) {
        on_timeout();
      }
    });
    return;
  }
  if (post_io) {
    auto poll = std::make_shared<std::function<void()>>();
    *poll = [post_io, settled, deadline, on_timeout = std::move(on_timeout), poll]() mutable {
      if (settled->load(std::memory_order_acquire)) {
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        if (on_timeout) {
          on_timeout();
        }
        return;
      }
      post_io([poll, settled]() {
        if (!settled->load(std::memory_order_acquire)) {
          (*poll)();
        }
      });
    };
    post_io([poll]() { (*poll)(); });
    return;
  }
  AmpParkUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline, io_pump);
  if (!settled->load(std::memory_order_acquire) && on_timeout) {
    on_timeout();
  }
}

} // namespace pbr
