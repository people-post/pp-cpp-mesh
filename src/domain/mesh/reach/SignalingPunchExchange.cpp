#include "domain/mesh/reach/SignalingPunchExchange.h"

#include "common/Utilities.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

SignalingPunchExchange::SignalingPunchExchange() {
  redirectLogger("SignalingPunchExchange");
}

void SignalingPunchExchange::Complete(const std::string& epoch_id, Roe<void> result) {
  if (!pending_ || pending_->epoch_id != epoch_id) {
    return;
  }
  auto done = std::move(pending_->done);
  pending_.reset();
  if (done) {
    done(std::move(result));
  }
}

void SignalingPunchExchange::Request(const std::string& target_peer_id, std::vector<std::string> my_addrs,
                                     DoneFn done) {
  if (!done) {
    return;
  }
  if (my_addrs.empty()) {
    done(Error("signaling punch: no local candidates"));
    return;
  }
  if (!ports_.send_offer) {
    done(Error("signaling punch: no carrier"));
    return;
  }
  PunchSignal offer;
  offer.epoch_id = util::GenerateUuid();
  offer.window_ms = kDefaultWindowMs;
  offer.addrs = std::move(my_addrs);
  if (ports_.local_peer_id) {
    offer.peer_id = ports_.local_peer_id();
  }
  if (offer.peer_id.empty()) {
    offer.peer_id = target_peer_id;
  }
  // A request whose offer cannot go out fails alone: the punch already in flight stays.
  if (auto sent = ports_.send_offer(offer); !sent) {
    done(sent.error());
    return;
  }
  if (pending_) {
    Complete(pending_->epoch_id, Error("signaling punch: superseded"));
  }
  pending_ = Pending{offer.epoch_id, std::move(done)};
  log().info << "punch offer sent epoch=" << offer.epoch_id << " addrs=" << offer.addrs.size();
}

Roe<void> SignalingPunchExchange::OnOffer(const PunchSignal& offer, const std::string& sender_key) {
  if (!ports_.burst) {
    return Error("signaling punch burst unavailable");
  }
  if (ports_.may_answer && !ports_.may_answer(offer.peer_id, sender_key)) {
    return Error("signaling punch: declined (address not disclosed to this peer)");
  }
  std::vector<std::string> my_addrs = ports_.local_candidates ? ports_.local_candidates() : std::vector<std::string>{};
  if (my_addrs.empty()) {
    return Error("signaling punch: no local candidates for answer");
  }
  if (ports_.register_listen) {
    ports_.register_listen(!offer.peer_id.empty() ? offer.peer_id : sender_key, offer.addrs);
  }
  PunchSignal answer;
  answer.epoch_id = offer.epoch_id;
  answer.window_ms = offer.window_ms > 0 ? offer.window_ms : kDefaultWindowMs;
  answer.addrs = std::move(my_addrs);
  if (ports_.local_peer_id) {
    answer.peer_id = ports_.local_peer_id();
  }
  if (!ports_.send_answer) {
    return Error("signaling punch: no carrier");
  }
  if (auto sent = ports_.send_answer(answer); !sent) {
    return sent.error();
  }
  log().info << "punch answer sent epoch=" << offer.epoch_id << " peer_addrs=" << offer.addrs.size();
  ports_.burst(offer.addrs, answer.window_ms, [this, epoch = offer.epoch_id](Roe<void> r) {
    if (!r) {
      log().warning << "punch answer-side burst failed epoch=" << epoch << " err=" << r.error().message;
    } else {
      log().info << "punch answer-side burst ok epoch=" << epoch;
    }
  });
  return {};
}

Roe<void> SignalingPunchExchange::OnAnswer(const PunchSignal& answer) {
  if (!pending_ || pending_->epoch_id != answer.epoch_id) {
    log().info << "punch answer ignored; no pending epoch=" << answer.epoch_id;
    return {};
  }
  if (!ports_.burst) {
    Complete(answer.epoch_id, Error("signaling punch burst unavailable"));
    return Error("signaling punch burst unavailable");
  }
  if (ports_.register_listen && !answer.peer_id.empty()) {
    ports_.register_listen(answer.peer_id, answer.addrs);
  }
  const int window = answer.window_ms > 0 ? answer.window_ms : kDefaultWindowMs;
  ports_.burst(answer.addrs, window, [this, epoch = answer.epoch_id](Roe<void> r) {
    Complete(epoch, std::move(r));
  });
  return {};
}

} // namespace pbr
