#pragma once

#include <chrono>
#include <functional>
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

} // namespace pbr
