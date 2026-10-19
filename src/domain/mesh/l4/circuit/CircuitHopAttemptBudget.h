#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

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
/** Nested Establish after a successful bridge ack (clamped by remaining envelope). */
inline constexpr int64_t kCircuitNestedEstablishBaseMs = 8000;

/** Reorder: sticky first (if present), then Connected, then the rest — stable within bands. */
inline std::vector<std::string> OrderCircuitRelayAttempts(
    std::vector<std::string> relays, const std::string& sticky,
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

/** Hop answered quickly with a terminal miss — advance without burning WaitAck. */
inline bool CircuitBridgeErrorIsFastFail(std::string_view message) {
  // `bridge timed out` = dialer WaitAck with no hop ack (Preferred hang / far-leg race /
  // lost not-reg). Sticky-once lets answerer finish seed park on the same hop (H010).
  return message.find("not registered") != std::string_view::npos ||
         message.find("not dialable") != std::string_view::npos ||
         message.find("endpoint not") != std::string_view::npos ||
         message.find("undialable") != std::string_view::npos ||
         message.find("relay is target") != std::string_view::npos ||
         message.find("!endpoint") != std::string_view::npos ||
         message.find("bridge timed out") != std::string_view::npos;
}

/**
 * `not registered`: hop event-waited and answerer still parking — retry **this** relay
 * once, then advance so a second seed can be tried (B27). `bridges_started` already
 * includes the failed attempt (1 after first miss → allow one same-relay retry).
 */
inline bool CircuitShouldRetrySameRelayOnNotReg(const bool not_reg,
                                                const std::size_t bridges_started) {
  return not_reg && bridges_started == 1;
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

} // namespace pbr
