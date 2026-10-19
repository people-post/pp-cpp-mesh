#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * H011: the rendezvous relay (R1) our circuit reach chose, told to the peer so its late reserve
 * prefers the same relay — and the peer's R1 taken in. Our R1 is often chosen before a carrier to
 * the peer exists (circuit path before the invite): it is kept and sent on Flush. Not thread-safe:
 * one owner thread.
 */
class CircuitR1Hint {
public:
  struct Ports {
    /** Carrier: tell the peer our R1; false when there is no peer to tell yet (kept pending). */
    std::function<bool(const std::string& r1)> send;
    /** The peer's R1: prefer it for our late reserve. */
    std::function<void(const std::string& r1)> prefer_late_reserve;
  };

  void SetPorts(Ports ports) { ports_ = std::move(ports); }

  void Announce(const std::string& r1) {
    if (r1.empty()) {
      return;
    }
    if (ports_.send && ports_.send(r1)) {
      pending_.clear();
    } else {
      pending_ = r1;
    }
  }
  /** A carrier to the peer may exist now: send the kept R1. */
  void Flush() {
    if (!pending_.empty()) {
      const std::string r1 = pending_;
      Announce(r1);
    }
  }
  void OnInbound(const std::string& r1) {
    if (!r1.empty() && ports_.prefer_late_reserve) {
      ports_.prefer_late_reserve(r1);
    }
  }
  const std::string& Pending() const { return pending_; }

private:
  Ports ports_;
  std::string pending_;
};

} // namespace pbr
