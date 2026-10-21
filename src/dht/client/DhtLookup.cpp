#include "mesh/dht/client/DhtLookup.h"

#include <algorithm>

namespace pp::mesh {

void DhtLookup::Run(const std::string& target_peer_id, std::vector<DhtLookupCandidate> seeds, const Limits limits,
                    Ask ask, Skip skip, Done done) {
  auto lookup = std::make_shared<DhtLookup>(target_peer_id, limits, std::move(ask), std::move(skip), std::move(done));
  {
    std::lock_guard lock(lookup->mutex_);
    for (DhtLookupCandidate& seed : seeds) {
      lookup->AddLocked(std::move(seed));
    }
  }
  lookup->Pump();
}

DhtLookup::DhtLookup(const std::string& target_peer_id, const Limits limits, Ask ask, Skip skip, Done done)
    : target_key_(DhtKeyOfPeerId(target_peer_id)),
      limits_(limits),
      ask_(std::move(ask)),
      skip_(std::move(skip)),
      done_(std::move(done)),
      deadline_(std::chrono::steady_clock::now() + limits.budget) {
  limits_.alpha = std::max<size_t>(limits_.alpha, 1);
  limits_.k = std::max<size_t>(limits_.k, 1);
}

void DhtLookup::AddLocked(DhtLookupCandidate candidate) {
  if (candidate.peer_key.empty() || seen_.contains(candidate.peer_key)) {
    return;
  }
  seen_.insert(candidate.peer_key);
  if (!candidate.key) {
    candidate.key = DhtKeyOfPeerId(candidate.peer_key);
  }
  // Keyed entries by distance to the target; unkeyed ones after them, in arrival order.
  auto before = [&](const Entry& e) {
    if (!candidate.key || !target_key_) {
      return true;
    }
    return e.candidate.key && !DhtCloser(*target_key_, *candidate.key, *e.candidate.key);
  };
  auto at = std::find_if_not(shortlist_.begin(), shortlist_.end(), before);
  shortlist_.insert(at, Entry{std::move(candidate)});
}

bool DhtLookup::StopIssuingLocked() const {
  return finished_ || best_self_ || best_other_ || asked_ >= limits_.max_asks ||
         std::chrono::steady_clock::now() >= deadline_;
}

void DhtLookup::Pump() {
  std::vector<DhtLookupCandidate> to_ask;
  bool finish = false;
  Outcome outcome;
  {
    std::lock_guard lock(mutex_);
    if (finished_) {
      return;
    }
    if (!StopIssuingLocked()) {
      // The k closest that have not failed; ask the fresh ones among them, closest first.
      size_t considered = 0;
      for (size_t i = 0; i < shortlist_.size() && considered < limits_.k; ++i) {
        Entry& entry = shortlist_[i];
        if (entry.state == State::Failed) {
          continue;
        }
        ++considered;
        if (entry.state != State::Fresh || inflight_ >= limits_.alpha || asked_ >= limits_.max_asks) {
          continue;
        }
        entry.state = State::Asking;
        ++inflight_;
        ++asked_;
        to_ask.push_back(entry.candidate);
      }
    }
    if (to_ask.empty() && inflight_ == 0) {
      finished_ = finish = true;
      outcome.record = best_self_ ? best_self_ : best_other_;
      outcome.asked = asked_;
      outcome.answered = answered_;
    }
  }
  if (finish) {
    if (done_) {
      done_(std::move(outcome));
    }
    return;
  }
  auto self = shared_from_this();
  for (const DhtLookupCandidate& candidate : to_ask) {
    ask_(candidate, [self, key = candidate.peer_key](std::optional<DhtLookupAnswer> answer) {
      self->OnAnswer(key, std::move(answer));
    });
  }
}

void DhtLookup::OnAnswer(const std::string& peer_key, std::optional<DhtLookupAnswer> answer) {
  {
    std::lock_guard lock(mutex_);
    // Entries shift as hints arrive: find ours by key.
    const auto it = std::find_if(shortlist_.begin(), shortlist_.end(),
                                 [&](const Entry& e) { return e.candidate.peer_key == peer_key; });
    if (it != shortlist_.end() && it->state == State::Asking) {
      it->state = answer ? State::Answered : State::Failed;
    }
    if (inflight_ > 0) {
      --inflight_;
    }
    if (answer) {
      ++answered_;
      if (answer->record) {
        std::optional<PeerRoutingRecord>& slot = answer->self_answer ? best_self_ : best_other_;
        if (!slot || answer->record->seq > slot->seq) {
          slot = std::move(answer->record);
        }
      }
      if (limits_.follow_hints) {
        for (DhtCloserPeer& hint : answer->closer) {
          if (skip_ && skip_(hint.peer_id)) {
            continue;
          }
          AddLocked(DhtLookupCandidate{hint.peer_id, std::nullopt, std::move(hint.multiaddr)});
        }
      }
    }
  }
  Pump();
}

} // namespace pp::mesh
