#pragma once

#include "mesh/reach/PeerReachRecord.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pp::mesh {

/**
 * Choosing peers for a job from the record (docs/PEER_REACH.md § 5): enough live ones, a few cold
 * dials a round, none spent on a peer known gone.
 */

/** Rendezvous: how many live relays are enough, and the cold dials one round may spend. */
inline constexpr std::size_t kRendezvousLiveTarget = 2;
inline constexpr std::size_t kReachMaxColdDialsPerRound = 3;

/** One candidate as selection sees it; the order they are given in is the policy's. */
struct ReachCandidate {
  std::string peer_id;
  /** A link to it is up now. */
  bool connected = false;
  PeerReachFacts facts;
};

struct ReachSelection {
  /** Up now, in policy order: used as they are. */
  std::vector<std::string> connected;
  /**
   * To dial, best first: `must_dial`, then the record's Live (latest success first), then the rest
   * in policy order. At most `max_cold`; only `must_dial` once `connected` meets the target.
   */
  std::vector<std::string> cold;
  /** Down before their retry time: not dialed for the job. */
  std::vector<std::string> down;
};

/** The record says not to dial it for a job now. */
inline bool ReachHeldDown(const PeerReachFacts& facts, const int64_t now_ms) {
  return facts.state == PeerReachState::Down && now_ms < facts.retry_at_ms;
}

/**
 * `must_dial` (e.g. the relay a call chose) leads the cold list whatever the target, unless it is
 * held down. A link that is up counts whatever the record says.
 */
inline ReachSelection SelectForRole(const std::vector<ReachCandidate>& ordered, const int64_t now_ms,
                                    const std::size_t live_target, const std::size_t max_cold,
                                    const std::string& must_dial = {}) {
  ReachSelection out;
  std::vector<const ReachCandidate*> live;
  std::vector<const ReachCandidate*> rest;
  const ReachCandidate* first = nullptr;
  for (const ReachCandidate& c : ordered) {
    if (c.peer_id.empty()) {
      continue;
    }
    if (c.connected) {
      out.connected.push_back(c.peer_id);
    } else if (ReachHeldDown(c.facts, now_ms)) {
      out.down.push_back(c.peer_id);
    } else if (!must_dial.empty() && c.peer_id == must_dial) {
      first = &c;
    } else if (c.facts.state == PeerReachState::Live) {
      live.push_back(&c);
    } else {
      rest.push_back(&c);
    }
  }
  std::stable_sort(live.begin(), live.end(), [](const ReachCandidate* a, const ReachCandidate* b) {
    return a->facts.last_ok_ms > b->facts.last_ok_ms;
  });
  if (first) {
    out.cold.push_back(first->peer_id);
  }
  if (out.connected.size() >= live_target) {
    return out;
  }
  for (const auto* band : {&live, &rest}) {
    for (const ReachCandidate* c : *band) {
      if (out.cold.size() >= max_cold) {
        return out;
      }
      out.cold.push_back(c->peer_id);
    }
  }
  return out;
}

/**
 * How many seeds a park needs Connected: the target, or every seed not held down when there are
 * fewer. 0 when every seed is held down — the park cannot land.
 */
inline std::size_t SeedParkNeeds(const std::size_t seeds_not_down) {
  return std::min(kRendezvousLiveTarget, seeds_not_down);
}

/**
 * A park redials a seed whose dial just missed only when that miss may pass: the record does not
 * hold it down, or it refused (up, busy for a moment). A seed that did not answer a whole dial is
 * not dialed again for the same park.
 */
inline bool SeedMissMayPass(const PeerReachFacts& facts, const int64_t now_ms) {
  return !ReachHeldDown(facts, now_ms) || facts.last_failure == PeerReachFailure::Refused;
}

} // namespace pp::mesh
