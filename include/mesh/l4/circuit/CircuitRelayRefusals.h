#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::mesh {

/** A relay that refused a circuit (not ready, out of scope) is tried last for this long. */
inline constexpr int64_t kCircuitRelayRefusedHoldMs = 10 * 60 * 1000;

/**
 * Relays that refused us lately (pp-browser #379: a peer that is no relay answered "service not ready"
 * to every try and kept its place at the head of the list). A refusal demotes the relay behind every
 * other until the hold ends or it serves a bridge; it is not dropped, so a relay that was only starting
 * is still tried when nothing else is left. Thread-safe.
 */
class CircuitRelayRefusals {
public:
  void Note(const std::string& relay, const int64_t now_ms) {
    std::lock_guard lock(mu_);
    until_ms_[relay] = now_ms + kCircuitRelayRefusedHoldMs;
  }

  void Clear(const std::string& relay) {
    std::lock_guard lock(mu_);
    until_ms_.erase(relay);
  }

  bool Refused(const std::string& relay, const int64_t now_ms) const {
    std::lock_guard lock(mu_);
    const auto it = until_ms_.find(relay);
    return it != until_ms_.end() && it->second > now_ms;
  }

  /** `relays` with the ones that refused lately moved behind the rest, each part in its order. */
  std::vector<std::string> Demote(std::vector<std::string> relays, const int64_t now_ms) {
    std::lock_guard lock(mu_);
    std::erase_if(until_ms_, [now_ms](const auto& entry) { return entry.second <= now_ms; });
    std::stable_partition(relays.begin(), relays.end(),
                          [this](const std::string& relay) { return !until_ms_.contains(relay); });
    return relays;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, int64_t> until_ms_;
};

} // namespace pp::mesh
