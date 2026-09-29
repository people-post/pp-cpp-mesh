#pragma once

#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** One side's punch candidates in a signaled punch (H012): offer or answer. */
struct PunchSignal {
  std::string epoch_id;
  int window_ms = 0;
  std::vector<std::string> addrs;
  /** The sender's mesh PeerId (offer: initiator; answer: target). */
  std::string peer_id;
};

/**
 * A hole punch whose candidates travel over a signaling carrier instead of an Amp introducer (H012 —
 * the introducers are exhausted). The initiator offers its candidates under an epoch; the target
 * answers with its own and bursts; the initiator bursts on the answer and the epoch completes with
 * the burst's result. One pending epoch: a new request supersedes it. The carrier (e.g. call-control)
 * only moves the signals. Not thread-safe: one owner thread.
 */
class SignalingPunchExchange : public Module {
public:
  static constexpr int kDefaultWindowMs = 2000;
  using DoneFn = std::function<void(Roe<void>)>;

  struct Ports {
    /** Carrier: deliver our offer / answer to the peer. */
    std::function<Roe<void>(const PunchSignal& offer)> send_offer;
    std::function<Roe<void>(const PunchSignal& answer)> send_answer;
    /** Dial the peer's candidates inside `window_ms`. */
    std::function<void(const std::vector<std::string>& peer_addrs, int window_ms, DoneFn done)> burst;
    /** Remember the peer's candidates as its listen addrs (keyed by PeerId, else the carrier's key). */
    std::function<void(const std::string& key, const std::vector<std::string>& addrs)> register_listen;
    /** Our candidates for an answer (punch candidates, else listen addrs). */
    std::function<std::vector<std::string>()> local_candidates;
    std::function<std::string()> local_peer_id;
  };

  SignalingPunchExchange();
  void SetPorts(Ports ports) { ports_ = std::move(ports); }

  /** Offer `my_addrs` to the peer (whose PeerId is `target_peer_id`); `done` runs when the epoch ends. */
  void Request(const std::string& target_peer_id, std::vector<std::string> my_addrs, DoneFn done);
  /** The peer's offer: answer with our candidates, then burst at theirs. */
  Roe<void> OnOffer(const PunchSignal& offer, const std::string& sender_key);
  /** The peer's answer to our pending epoch: burst at its candidates; the burst ends the epoch. */
  Roe<void> OnAnswer(const PunchSignal& answer);

  bool HasPending() const { return pending_.has_value(); }

private:
  struct Pending {
    std::string epoch_id;
    DoneFn done;
  };
  void Complete(const std::string& epoch_id, Roe<void> result);

  Ports ports_;
  std::optional<Pending> pending_;
};

} // namespace pbr
