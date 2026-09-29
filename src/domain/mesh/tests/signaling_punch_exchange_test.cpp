#include "domain/mesh/reach/CircuitR1Hint.h"
#include "domain/mesh/reach/SignalingPunchExchange.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** Two sides whose carrier is a queue: offers / answers are delivered when the test pumps. */
struct Side {
  SignalingPunchExchange exchange;
  std::vector<std::string> candidates;
  std::string peer_id;
  std::optional<PunchSignal> sent_offer;
  std::optional<PunchSignal> sent_answer;
  std::vector<std::vector<std::string>> bursts;
  std::map<std::string, std::vector<std::string>> registered;
  SignalingPunchExchange::DoneFn pending_burst;
  bool carrier_up = true;

  void Bind() {
    SignalingPunchExchange::Ports ports;
    ports.send_offer = [this](const PunchSignal& offer) -> Roe<void> {
      if (!carrier_up) {
        return Error("no carrier");
      }
      sent_offer = offer;
      return {};
    };
    ports.send_answer = [this](const PunchSignal& answer) -> Roe<void> {
      sent_answer = answer;
      return {};
    };
    ports.burst = [this](const std::vector<std::string>& addrs, int, SignalingPunchExchange::DoneFn done) {
      bursts.push_back(addrs);
      pending_burst = std::move(done);
    };
    ports.register_listen = [this](const std::string& key, const std::vector<std::string>& addrs) {
      registered[key] = addrs;
    };
    ports.local_candidates = [this]() { return candidates; };
    ports.local_peer_id = [this]() { return peer_id; };
    exchange.SetPorts(std::move(ports));
  }
};

// H012: offer → answer + answer-side burst → initiator burst → the epoch completes with the burst's result.
TEST(SignalingPunchExchangeTest, OfferAnswerBurstCompletesTheEpoch) {
  Side a;
  Side b;
  a.peer_id = "12D3a";
  b.peer_id = "12D3b";
  b.candidates = {"/ip4/2.2.2.2/tcp/2"};
  a.Bind();
  b.Bind();

  std::optional<bool> result;
  a.exchange.Request("12D3b", {"/ip4/1.1.1.1/tcp/1"}, [&](Roe<void> r) { result = static_cast<bool>(r); });
  ASSERT_TRUE(a.sent_offer);
  EXPECT_EQ(a.sent_offer->peer_id, "12D3a");
  EXPECT_EQ(a.sent_offer->window_ms, SignalingPunchExchange::kDefaultWindowMs);

  ASSERT_TRUE(b.exchange.OnOffer(*a.sent_offer, "account:a"));
  ASSERT_TRUE(b.sent_answer);
  EXPECT_EQ(b.sent_answer->epoch_id, a.sent_offer->epoch_id);
  EXPECT_EQ(b.registered["12D3a"], a.sent_offer->addrs) << "the offer's candidates are the peer's listen addrs";
  ASSERT_EQ(b.bursts.size(), 1u);
  EXPECT_EQ(b.bursts[0], a.sent_offer->addrs);

  ASSERT_TRUE(a.exchange.OnAnswer(*b.sent_answer));
  EXPECT_EQ(a.registered["12D3b"], b.candidates);
  ASSERT_EQ(a.bursts.size(), 1u);
  EXPECT_FALSE(result) << "the epoch ends with the burst, not the answer";
  a.pending_burst(Roe<void>{});
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);
  EXPECT_FALSE(a.exchange.HasPending());
}

TEST(SignalingPunchExchangeTest, NewRequestSupersedesAndStaleAnswersAreIgnored) {
  Side a;
  a.Bind();
  std::vector<std::string> outcomes;
  a.exchange.Request("12D3b", {"/ip4/1.1.1.1/tcp/1"},
                     [&](Roe<void> r) { outcomes.push_back(r ? "ok" : r.error().message); });
  const PunchSignal first = *a.sent_offer;
  a.exchange.Request("12D3b", {"/ip4/1.1.1.1/tcp/1"}, [&](Roe<void> r) { outcomes.push_back(r ? "ok" : "second"); });
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_NE(outcomes[0].find("superseded"), std::string::npos);

  PunchSignal stale = first;
  stale.addrs = {"/ip4/9.9.9.9/tcp/9"};
  EXPECT_TRUE(a.exchange.OnAnswer(stale));
  EXPECT_TRUE(a.bursts.empty()) << "an answer to a superseded epoch bursts nothing";
  EXPECT_TRUE(a.exchange.HasPending());
}

TEST(SignalingPunchExchangeTest, FailsFastWithoutCandidatesOrCarrier) {
  Side a;
  a.Bind();
  std::optional<std::string> err;
  a.exchange.Request("12D3b", {}, [&](Roe<void> r) { err = r ? "" : r.error().message; });
  ASSERT_TRUE(err);
  EXPECT_NE(err->find("no local candidates"), std::string::npos);

  a.carrier_up = false;
  err.reset();
  a.exchange.Request("12D3b", {"/ip4/1.1.1.1/tcp/1"}, [&](Roe<void> r) { err = r ? "" : r.error().message; });
  ASSERT_TRUE(err);
  EXPECT_EQ(*err, "no carrier");
  EXPECT_FALSE(a.exchange.HasPending());

  PunchSignal offer;
  offer.epoch_id = "e1";
  EXPECT_FALSE(a.exchange.OnOffer(offer, "account:x")) << "no candidates to answer with";
  EXPECT_FALSE(a.sent_answer);
}

// H011: an R1 chosen before there is a peer to tell is kept and sent on Flush.
TEST(CircuitR1HintTest, KeepsTheR1UntilACarrierExists) {
  CircuitR1Hint hint;
  bool peer = false;
  std::vector<std::string> sent;
  std::vector<std::string> preferred;
  hint.SetPorts({[&](const std::string& r1) {
                   if (!peer) {
                     return false;
                   }
                   sent.push_back(r1);
                   return true;
                 },
                 [&](const std::string& r1) { preferred.push_back(r1); }});
  hint.Announce("12D3r1");
  EXPECT_TRUE(sent.empty());
  EXPECT_EQ(hint.Pending(), "12D3r1");
  peer = true;
  hint.Flush();
  EXPECT_EQ(sent, std::vector<std::string>{"12D3r1"});
  EXPECT_TRUE(hint.Pending().empty());
  hint.Flush();
  EXPECT_EQ(sent.size(), 1u) << "nothing left to flush";

  hint.OnInbound("12D3their");
  EXPECT_EQ(preferred, std::vector<std::string>{"12D3their"});
}

} // namespace
} // namespace pbr
