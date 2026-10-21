#pragma once

#include "mesh/dht/DhtKey.h"
#include "mesh/dht/DhtTypes.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pp::mesh {

/** Someone to ask in a lookup: a dial key, its DHT key when it is a PeerId, and a hinted address. */
struct DhtLookupCandidate {
  std::string peer_key;
  std::optional<DhtKey> key;
  /** From a `closer_peers` hint or the routing table; empty for the configured query peers. */
  std::string hint_addr;
};

/** What one asked peer answered, already checked by the asker. */
struct DhtLookupAnswer {
  /** A record for the target that verified (signature, PeerId binding, not expired). */
  std::optional<PeerRoutingRecord> record;
  /** The record came from its own subject (beats any other at any seq). */
  bool self_answer = false;
  std::vector<DhtCloserPeer> closer;
};

/**
 * One iterative Kademlia lookup (docs/DHT.md § 4.4): a shortlist ordered by XOR distance to the
 * target; `alpha` asks in flight to the closest not yet asked; `closer_peers` hints join the
 * shortlist (deduplicated, minus `skip`). It stops issuing once a valid record has arrived, the k
 * closest known have all answered or failed, `max_asks` were sent, or `budget` has passed — and
 * finishes when the asks in flight settle, with the best record (a self answer first, else the
 * highest seq).
 *
 * Transport-free: `ask` does the RPC and the checks. It may answer inline. Thread-safe.
 */
class DhtLookup : public std::enable_shared_from_this<DhtLookup> {
public:
  struct Limits {
    size_t alpha = 3;
    size_t k = 20;
    size_t max_asks = 30;
    std::chrono::milliseconds budget{8000};
    /** Off: ask only the seeds (a Client never walks strangers — DHT.md D2). */
    bool follow_hints = true;
  };
  struct Outcome {
    std::optional<PeerRoutingRecord> record;
    size_t asked = 0;
    size_t answered = 0;
  };
  using AnswerCb = std::function<void(std::optional<DhtLookupAnswer>)>;
  using Ask = std::function<void(const DhtLookupCandidate&, AnswerCb)>;
  using Skip = std::function<bool(const std::string& peer_id)>;
  using Done = std::function<void(Outcome)>;

  static void Run(const std::string& target_peer_id, std::vector<DhtLookupCandidate> seeds, Limits limits, Ask ask,
                  Skip skip, Done done);

  /** Use `Run`. */
  DhtLookup(const std::string& target_peer_id, Limits limits, Ask ask, Skip skip, Done done);

private:
  enum class State { Fresh, Asking, Answered, Failed };
  struct Entry {
    DhtLookupCandidate candidate;
    State state = State::Fresh;
  };

  void AddLocked(DhtLookupCandidate candidate);
  void Pump();
  void OnAnswer(const std::string& peer_key, std::optional<DhtLookupAnswer> answer);
  bool StopIssuingLocked() const;

  std::optional<DhtKey> target_key_;
  Limits limits_;
  Ask ask_;
  Skip skip_;
  Done done_;
  std::chrono::steady_clock::time_point deadline_;

  std::mutex mutex_;
  /** Closest first; unkeyed entries (non-PeerId dial keys) last, in arrival order. */
  std::vector<Entry> shortlist_;
  std::unordered_set<std::string> seen_;  // peer_keys ever added (dedupe)
  size_t inflight_ = 0;
  size_t asked_ = 0;
  size_t answered_ = 0;
  std::optional<PeerRoutingRecord> best_self_;
  std::optional<PeerRoutingRecord> best_other_;
  bool finished_ = false;
};

} // namespace pp::mesh
