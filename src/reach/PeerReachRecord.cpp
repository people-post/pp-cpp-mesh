#include "mesh/reach/PeerReachRecord.h"

#include <algorithm>
#include <chrono>

namespace pp::mesh {
namespace {

PeerLinkKind PathOf(const pp::amp::LinkPathKind kind) {
  switch (kind) {
  case pp::amp::LinkPathKind::Direct:
    return PeerLinkKind::Direct;
  case pp::amp::LinkPathKind::Punched:
    return PeerLinkKind::Punched;
  case pp::amp::LinkPathKind::Carrier:
    return PeerLinkKind::Relayed;
  }
  return PeerLinkKind::Unknown;
}

/** A failed dial names its peer by the dial book's PeerId, else by a dial key that is one (aliases carry a scheme). */
std::string FailedPeer(const pp::amp::LinkEvent& event) {
  if (!event.peer_id.empty()) {
    return event.peer_id;
  }
  return event.dial_key.find(':') == std::string::npos ? event.dial_key : std::string();
}

PeerReachFailure DialFailure(const pp::amp::LinkDropReason reason) {
  switch (reason) {
  case pp::amp::LinkDropReason::HandshakeFailed:
    return PeerReachFailure::Refused;
  case pp::amp::LinkDropReason::TransportFailed:
    return PeerReachFailure::Unreachable;
  case pp::amp::LinkDropReason::DualDialLost:
    return PeerReachFailure::None;  // the peer answered on the other dial
  default:
    return PeerReachFailure::NoAnswer;
  }
}

/** A connected link's drop that says the peer is gone; most drops (closed, idle, ours) say nothing. */
PeerReachFailure LossFailure(const pp::amp::LinkEvent& event) {
  if (event.reason == pp::amp::LinkDropReason::ConnectionDead && event.keepalive_interval_ms > 0) {
    return PeerReachFailure::KeepaliveLost;
  }
  if (event.reason == pp::amp::LinkDropReason::TransportFailed) {
    return PeerReachFailure::Unreachable;
  }
  return PeerReachFailure::None;
}

} // namespace

const char* PeerReachStateName(const PeerReachState state) {
  switch (state) {
  case PeerReachState::Unknown:
    return "unknown";
  case PeerReachState::Live:
    return "live";
  case PeerReachState::Down:
    return "down";
  }
  return "unknown";
}

const char* PeerReachFailureName(const PeerReachFailure failure) {
  switch (failure) {
  case PeerReachFailure::None:
    return "none";
  case PeerReachFailure::NoAnswer:
    return "no-answer";
  case PeerReachFailure::Refused:
    return "refused";
  case PeerReachFailure::KeepaliveLost:
    return "keepalive-lost";
  case PeerReachFailure::Unreachable:
    return "unreachable";
  }
  return "none";
}

int64_t PeerReachRecord::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

PeerReachRecord::Entry& PeerReachRecord::Touch(const std::string& peer_id, const int64_t now_ms) {
  auto it = entries_.find(peer_id);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxPeers) {
      auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
        return a.second.touched_ms < b.second.touched_ms;
      });
      entries_.erase(oldest);
    }
    it = entries_.emplace(peer_id, Entry{}).first;
  }
  it->second.touched_ms = now_ms;
  return it->second;
}

void PeerReachRecord::NoteConnected(const std::string& peer_id, const PeerLinkKind path, const int64_t now_ms) {
  if (peer_id.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  PeerReachFacts& f = Touch(peer_id, now_ms).facts;
  f.state = PeerReachState::Live;
  f.last_ok_ms = now_ms;
  f.last_ok_path = path;
  f.down_since_ms = 0;
  f.retry_at_ms = 0;
  f.fails_in_row = 0;
}

void PeerReachRecord::NoteFailed(const std::string& peer_id, const PeerReachFailure failure, const int64_t now_ms,
                                 const uint64_t network_gen) {
  if (peer_id.empty() || failure == PeerReachFailure::None) {
    return;
  }
  std::lock_guard lock(mu_);
  Entry& entry = Touch(peer_id, now_ms);
  PeerReachFacts& f = entry.facts;
  if (f.state != PeerReachState::Down || entry.down_network_gen != network_gen) {
    f.down_since_ms = now_ms;
    f.fails_in_row = 0;
  }
  ++f.fails_in_row;
  f.state = PeerReachState::Down;
  f.last_fail_ms = now_ms;
  f.last_failure = failure;
  entry.down_network_gen = network_gen;
  if (failure == PeerReachFailure::Refused) {
    f.retry_at_ms = now_ms + kRetryRefusedMs;
  } else {
    const int doublings = std::min(f.fails_in_row - 1, 3);
    f.retry_at_ms = now_ms + std::min(kRetryFirstMs << doublings, kRetryMaxMs);
  }
}

void PeerReachRecord::OnLinkEvent(const pp::amp::LinkEvent& event, const int64_t now_ms, const uint64_t network_gen) {
  switch (event.kind) {
  case pp::amp::LinkEvent::Kind::Connected:
    NoteConnected(event.peer_id, PathOf(event.path_kind), now_ms);
    return;
  case pp::amp::LinkEvent::Kind::DialFailed:
    NoteFailed(FailedPeer(event), DialFailure(event.reason), now_ms, network_gen);
    return;
  case pp::amp::LinkEvent::Kind::Dropped:
    if (event.was_connected) {
      NoteFailed(event.peer_id, LossFailure(event), now_ms, network_gen);
    }
    return;
  case pp::amp::LinkEvent::Kind::PathChanged:
    return;
  }
}

PeerReachFacts PeerReachRecord::AsOf(const Entry& entry, const uint64_t network_gen) {
  PeerReachFacts f = entry.facts;
  if (f.state == PeerReachState::Down && entry.down_network_gen != network_gen) {
    f.state = PeerReachState::Unknown;  // judged on another network
    f.retry_at_ms = 0;
  }
  return f;
}

PeerReachFacts PeerReachRecord::Facts(const std::string& peer_id, const uint64_t network_gen) const {
  std::lock_guard lock(mu_);
  auto it = entries_.find(peer_id);
  return it == entries_.end() ? PeerReachFacts{} : AsOf(it->second, network_gen);
}

bool PeerReachRecord::MayDial(const std::string& peer_id, const int64_t now_ms, const uint64_t network_gen) const {
  const PeerReachFacts f = Facts(peer_id, network_gen);
  return f.state != PeerReachState::Down || now_ms >= f.retry_at_ms;
}

size_t PeerReachRecord::Size() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

} // namespace pp::mesh
