#pragma once

#include "mesh/l4/circuit/CircuitTunnelErr.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pp::mesh {

/**
 * First-connect circuit StartBridge spend limit (H010).
 * Ranked candidates are a queue with a budget — not an exhaustive search.
 */
/** Wall clock for EnsureViaCircuit — covers answerer seed park (~12s) + nest Establish. */
inline constexpr int64_t kCircuitReachEnvelopeMs = 14000;
/** Cap per StartBridge WaitAck (further clamped by remaining envelope). */
inline constexpr int64_t kCircuitStartBridgeBaseMs = 4000;
/** Leave slack inside the envelope for nested call-media Establish after ack. */
inline constexpr int64_t kCircuitNestedSlackMs = 2000;
/** Stop starting new bridges when remaining wall time is below this. */
inline constexpr int64_t kCircuitMinUsefulTryMs = 2000;
/** Max StartBridge calls per EnsureViaCircuit (skips do not count). */
inline constexpr std::size_t kCircuitMaxStartBridgeAttempts = 4;
/**
 * StartBridges out at once per EnsureViaCircuit: the first two relays are bridged to together, and
 * the first to ack wins (the other is cancelled) — a silent first relay no longer costs a whole bridge
 * timeout before the second is tried (pp-browser #577). Costs a second bridge at a relay, briefly.
 */
inline constexpr std::size_t kCircuitParallelBridges = 2;
/** Nested Establish after a successful bridge ack (clamped by remaining envelope). */
inline constexpr int64_t kCircuitNestedEstablishBaseMs = 8000;

/** Reorder: sticky first (if present), then Connected, then the rest — stable within bands. */
inline std::vector<std::string> OrderCircuitRelayAttempts(std::vector<std::string> relays, const std::string& sticky,
                                                          const std::function<bool(const std::string&)>& is_connected) {
  if (relays.empty()) {
    return relays;
  }
  std::vector<std::string> ordered;
  ordered.reserve(relays.size());
  auto take = [&](const std::function<bool(const std::string&)>& pred) {
    std::vector<std::string> rest;
    rest.reserve(relays.size());
    for (std::string& key : relays) {
      if (pred(key)) {
        ordered.push_back(std::move(key));
      } else {
        rest.push_back(std::move(key));
      }
    }
    relays = std::move(rest);
  };
  if (!sticky.empty()) {
    take([&](const std::string& key) { return key == sticky; });
  }
  if (is_connected) {
    take([&](const std::string& key) { return is_connected(key); });
  }
  for (std::string& key : relays) {
    ordered.push_back(std::move(key));
  }
  return ordered;
}

/**
 * The relays the target says it holds reservations on (a call's CallAccept, H013) go first, in its
 * order, then the rest as ranked. Advertised relays the dialer cannot dial are not added: a
 * StartBridge needs the relay's endpoint.
 */
inline std::vector<std::string> PutAdvertisedRelaysFirst(std::vector<std::string> ranked,
                                                         const std::vector<std::string>& advertised) {
  if (advertised.empty()) {
    return ranked;
  }
  std::vector<std::string> ordered;
  ordered.reserve(ranked.size());
  for (const std::string& key : advertised) {
    auto it = std::find(ranked.begin(), ranked.end(), key);
    if (it != ranked.end()) {
      ordered.push_back(std::move(*it));
      ranked.erase(it);
    }
  }
  for (std::string& key : ranked) {
    ordered.push_back(std::move(key));
  }
  return ordered;
}

inline bool IsAdvertisedRelay(const std::vector<std::string>& advertised, const std::string& key) {
  return std::find(advertised.begin(), advertised.end(), key) != advertised.end();
}

/**
 * StartBridge timeout_ms for this attempt.
 * `remaining_ms` is wall time left in the EnsureViaCircuit envelope.
 * Nested call-media reserves nest slack so ack+Establish can finish inside the envelope.
 */
inline int64_t CircuitStartBridgeTimeoutMs(const int64_t remaining_ms, const bool nested_session) {
  if (remaining_ms < kCircuitMinUsefulTryMs) {
    return 0;
  }
  const int64_t reserve = nested_session ? kCircuitNestedSlackMs : 0;
  // Hold nest slack while several tries remain. On the last useful slice, spend remaining so
  // nested first-connect is not capped at ~2 StartBridges (dogfood not-reg / dual-NAT park).
  if (nested_session && remaining_ms <= reserve + kCircuitMinUsefulTryMs) {
    return std::min(kCircuitStartBridgeBaseMs, remaining_ms);
  }
  const int64_t usable = remaining_ms - reserve;
  if (usable < kCircuitMinUsefulTryMs) {
    return 0;
  }
  return std::min(kCircuitStartBridgeBaseMs, usable);
}

inline int64_t CircuitNestedEstablishTimeoutMs(const int64_t remaining_ms) {
  if (remaining_ms < kCircuitMinUsefulTryMs) {
    return 0;
  }
  return std::min(kCircuitNestedEstablishBaseMs, remaining_ms);
}

/**
 * A bridge that missed quickly with a terminal answer — worth one sticky retry, never a wait.
 * `Timeout` is the relay not acking in time (Preferred hang / far-leg race / a lost not-reserved):
 * sticky-once lets the answerer finish its seed park on the same relay (H010).
 */
inline bool CircuitBridgeMissIsFastFail(const CircuitTunnelErr code) {
  return code == CircuitTunnelErr::TargetNotReserved || code == CircuitTunnelErr::TargetUnreachable ||
         code == CircuitTunnelErr::RelayNoEndpoint || code == CircuitTunnelErr::Timeout;
}

/**
 * `not registered` from a relay the target advertised (H013): its reservation there may have just
 * lapsed or be renewing, so retry **this** relay once. A relay the target did not advertise is
 * advanced past at once — nothing would make the target appear there in time.
 */
inline bool CircuitShouldRetrySameRelayOnNotReg(const bool not_reg, const bool advertised_relay,
                                                const bool already_retried) {
  return not_reg && advertised_relay && !already_retried;
}

/**
 * After a StartBridge miss: retry the sticky hop once on other fast-fail (e.g. bridge timed
 * out), otherwise advance. `bridges_started` already includes the failed attempt.
 */
inline bool CircuitShouldRetryStickyOnce(const std::string& relay_key, const std::string& sticky,
                                         const bool sticky_retried, const bool fast_fail,
                                         const std::size_t bridges_started) {
  return fast_fail && !sticky.empty() && relay_key == sticky && !sticky_retried &&
         bridges_started < kCircuitMaxStartBridgeAttempts;
}

} // namespace pp::mesh
