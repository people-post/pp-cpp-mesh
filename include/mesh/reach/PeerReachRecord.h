#pragma once

#include "mesh/reach/PeerReachCoordinator.h"

#include "amp/link/LinkEvents.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pp::mesh {

/** What the record says about a peer (docs/PEER_REACH.md § 4). */
enum class PeerReachState : uint8_t {
  /** No outcome yet on this network. */
  Unknown,
  Live,
  /** It could not be reached; not dialed for a job before `retry_at_ms`. */
  Down,
};

/** Why a peer went Down. */
enum class PeerReachFailure : uint8_t {
  None,
  /** A dial got no answer (every candidate timed out). */
  NoAnswer,
  /** It answered and refused (link table full, handshake refused): up but busy — short retry. */
  Refused,
  /** A kept-alive link to it went silent (its echoes unanswered). */
  KeepaliveLost,
  /** The OS has no route to it. */
  Unreachable,
};

const char* PeerReachStateName(PeerReachState state);
const char* PeerReachFailureName(PeerReachFailure failure);

struct PeerReachFacts {
  PeerReachState state = PeerReachState::Unknown;
  int64_t last_ok_ms = 0;
  PeerLinkKind last_ok_path = PeerLinkKind::Unknown;
  int64_t last_fail_ms = 0;
  PeerReachFailure last_failure = PeerReachFailure::None;
  int64_t down_since_ms = 0;
  int64_t retry_at_ms = 0;
  int fails_in_row = 0;
};

/**
 * Facts the mesh observes about reaching each peer — fed by Amp's link events (Connected,
 * DialFailed, a kept-alive link going silent), nothing a peer reports. One record per mesh
 * (MeshConnectivity), bounded (oldest dropped past kMaxPeers), in memory. A Down mark belongs to
 * the network it was made on: after a local network change it reads Unknown. Thread-safe.
 */
class PeerReachRecord {
public:
  /** The clock the record's times are on (steady, ms); callers pass it as `now_ms`. */
  static int64_t NowMs();

  static constexpr size_t kMaxPeers = 512;
  static constexpr int64_t kRetryFirstMs = 5 * 60 * 1000;
  static constexpr int64_t kRetryMaxMs = 30 * 60 * 1000;
  static constexpr int64_t kRetryRefusedMs = 60 * 1000;

  void NoteConnected(const std::string& peer_id, PeerLinkKind path, int64_t now_ms);
  void NoteFailed(const std::string& peer_id, PeerReachFailure failure, int64_t now_ms, uint64_t network_gen);
  /** Apply an Amp link event (posted on the mesh io lane). */
  void OnLinkEvent(const pp::amp::LinkEvent& event, int64_t now_ms, uint64_t network_gen);

  /** As of `network_gen`: a Down mark from an earlier network reads Unknown. */
  PeerReachFacts Facts(const std::string& peer_id, uint64_t network_gen) const;
  /** Not Down, or Down past its retry time. */
  bool MayDial(const std::string& peer_id, int64_t now_ms, uint64_t network_gen) const;
  size_t Size() const;

private:
  struct Entry {
    PeerReachFacts facts;
    uint64_t down_network_gen = 0;
    int64_t touched_ms = 0;
  };

  Entry& Touch(const std::string& peer_id, int64_t now_ms);
  static PeerReachFacts AsOf(const Entry& entry, uint64_t network_gen);

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace pp::mesh
