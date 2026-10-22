// The iterative lookup over a simulated network (docs/DHT.md § 4.4, § 5).
#include "mesh/dht/client/DhtLookup.h"
#include "mesh/dht/DhtRoutingTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

std::string Pid(int i) {
  static const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::string tail;
  do {
    tail += alphabet[static_cast<size_t>(i % 58)];
    i /= 58;
  } while (i > 0);
  return "QmNode" + tail;
}

std::string Ma(int i) {
  return "/ip4/10." + std::to_string(i / 250) + "." + std::to_string(i % 250) + ".1/udp/1/adp/1.0.0/p2p/" + Pid(i);
}

/** n nodes, each with a full Kademlia table of the others; a record for `target` at its k closest. */
struct Network {
  struct Node {
    std::unique_ptr<DhtRoutingTable> table;
    bool holds_record = false;
    bool liar = false;  // answers with made-up hints, never the record
  };
  std::map<std::string, Node> nodes;
  std::string target;
  size_t answered_asks = 0;

  Network(int n, int target_index, size_t holders) {
    for (int i = 0; i < n; ++i) {
      nodes[Pid(i)].table = std::make_unique<DhtRoutingTable>(Pid(i));
    }
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < n; ++j) {
        (void)nodes[Pid(i)].table->NoteHeard(Pid(j), Ma(j), 0);
      }
    }
    target = Pid(target_index);
    std::vector<std::string> ids;
    for (const auto& [id, node] : nodes) {
      ids.push_back(id);
    }
    const DhtKey key = *DhtKeyOfPeerId(target);
    std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
      return DhtCloser(key, *DhtKeyOfPeerId(a), *DhtKeyOfPeerId(b));
    });
    for (size_t i = 0; i < holders && i < ids.size(); ++i) {
      nodes[ids[i]].holds_record = true;
    }
  }

  PeerRoutingRecord Record(int64_t seq = 1) const {
    PeerRoutingRecord record;
    record.peer_id = target;
    record.seq = seq;
    return record;
  }

  void Answer(const DhtLookupCandidate& asked, const DhtLookup::AnswerCb& answer) {
    const auto it = nodes.find(asked.peer_key);
    if (it == nodes.end()) {
      answer(std::nullopt);  // nobody there
      return;
    }
    ++answered_asks;
    DhtLookupAnswer out;
    if (it->second.liar) {
      for (int i = 0; i < 8; ++i) {
        const int ghost = 100000 + static_cast<int>(answered_asks) * 8 + i;  // a real-looking PeerId nobody has
        out.closer.push_back({Pid(ghost), Ma(ghost)});
      }
      answer(std::move(out));
      return;
    }
    if (it->second.holds_record || asked.peer_key == target) {
      out.record = Record();
      out.self_answer = asked.peer_key == target;
    }
    for (DhtContact& c : it->second.table->Closest(*DhtKeyOfPeerId(target), kDhtCloserPeers)) {
      out.closer.push_back({c.peer_id, c.multiaddr});
    }
    answer(std::move(out));
  }
};

DhtLookup::Outcome RunInline(Network& net, std::vector<DhtLookupCandidate> seeds, DhtLookup::Limits limits = {}) {
  std::optional<DhtLookup::Outcome> outcome;
  DhtLookup::Run(
      net.target, std::move(seeds), limits,
      [&](const DhtLookupCandidate& c, DhtLookup::AnswerCb answer) { net.Answer(c, answer); },
      [](const std::string&) { return false; }, [&](DhtLookup::Outcome o) { outcome = std::move(o); });
  EXPECT_TRUE(outcome.has_value()) << "an inline lookup finishes before Run returns";
  return outcome.value_or(DhtLookup::Outcome{});
}

/** Seeds: three nodes the asker happens to know, far from the target. */
std::vector<DhtLookupCandidate> FarSeeds(const Network& net) {
  std::vector<std::string> ids;
  for (const auto& [id, node] : net.nodes) {
    if (!node.holds_record && id != net.target) {
      ids.push_back(id);
    }
  }
  const DhtKey key = *DhtKeyOfPeerId(net.target);
  std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
    return DhtCloser(key, *DhtKeyOfPeerId(b), *DhtKeyOfPeerId(a));  // farthest first
  });
  return {{ids[0], std::nullopt, {}}, {ids[1], std::nullopt, {}}, {ids[2], std::nullopt, {}}};
}

TEST(DhtLookupTest, FindsARecordHeldNearItsKeyFromFarAway) {
  for (const int target : {7, 42, 133, 190}) {
    Network net(200, target, 20);
    const auto outcome = RunInline(net, FarSeeds(net));
    ASSERT_TRUE(outcome.record.has_value()) << "target " << target;
    EXPECT_EQ(outcome.record->peer_id, net.target);
    EXPECT_LE(outcome.asked, 12u) << "O(log n) asks, not a sweep";
  }
}

TEST(DhtLookupTest, NoRecordAnywhereEndsAfterTheClosestAnswered) {
  Network net(200, 5, 0);
  net.nodes.erase(net.target);  // the target is not in the network either
  const auto outcome = RunInline(net, FarSeeds(net));
  EXPECT_FALSE(outcome.record.has_value());
  EXPECT_GT(outcome.answered, 0u);
  EXPECT_LE(outcome.asked, DhtLookup::Limits{}.max_asks);
}

TEST(DhtLookupTest, LiarsWithMadeUpHintsDoNotStopIt) {
  Network net(200, 99, 20);
  int i = 0;
  for (auto& [id, node] : net.nodes) {
    if (!node.holds_record && id != net.target && i++ % 5 == 0) {
      node.liar = true;  // a fifth of the network
    }
  }
  const auto seeds = FarSeeds(net);
  net.nodes[seeds[0].peer_key].liar = true;  // two of the three seeds lie too
  net.nodes[seeds[1].peer_key].liar = true;
  net.nodes[seeds[2].peer_key].liar = false;
  const auto outcome = RunInline(net, seeds);
  ASSERT_TRUE(outcome.record.has_value());
  EXPECT_LE(outcome.asked, DhtLookup::Limits{}.max_asks);
}

TEST(DhtLookupTest, WithoutHintsOnlyTheSeedsAreAsked) {
  Network net(200, 99, 20);
  DhtLookup::Limits limits;
  limits.follow_hints = false;
  const auto outcome = RunInline(net, FarSeeds(net), limits);
  EXPECT_FALSE(outcome.record.has_value()) << "far seeds do not hold it";
  EXPECT_EQ(outcome.asked, 3u);
}

TEST(DhtLookupTest, AskedSeedsAreSkippedAndNonPeerIdKeysAreStillAsked) {
  Network net(50, 3, 20);
  std::vector<std::string> asked;
  std::optional<DhtLookup::Outcome> outcome;
  DhtLookup::Limits limits;
  limits.follow_hints = false;
  DhtLookup::Run(
      net.target, {{"seed", std::nullopt, {}}, {"seed", std::nullopt, {}}, {Pid(0), std::nullopt, {}}}, limits,
      [&](const DhtLookupCandidate& c, DhtLookup::AnswerCb answer) {
        asked.push_back(c.peer_key);
        answer(DhtLookupAnswer{});
      },
      {}, [&](DhtLookup::Outcome o) { outcome = std::move(o); });
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(asked.size(), 2u) << "the duplicate seed is asked once";
  EXPECT_EQ(std::count(asked.begin(), asked.end(), "seed"), 1);
}

// Answers arrive later (a real network): at most alpha asks are ever in flight, and a self
// answer wins over another's record with a higher seq.
TEST(DhtLookupTest, AtMostAlphaInFlightAndASelfAnswerWins) {
  Network net(200, 77, 20);
  std::deque<std::pair<DhtLookupCandidate, DhtLookup::AnswerCb>> pending;
  size_t max_inflight = 0;
  std::optional<DhtLookup::Outcome> outcome;
  auto seeds = FarSeeds(net);
  seeds.push_back({net.target, std::nullopt, {}});
  DhtLookup::Run(
      net.target, std::move(seeds), {},
      [&](const DhtLookupCandidate& c, DhtLookup::AnswerCb answer) {
        pending.emplace_back(c, std::move(answer));
        max_inflight = std::max(max_inflight, pending.size());
      },
      {}, [&](DhtLookup::Outcome o) { outcome = std::move(o); });
  while (!pending.empty()) {
    auto [c, answer] = std::move(pending.front());
    pending.pop_front();
    if (c.peer_key == net.target) {
      DhtLookupAnswer self;
      self.record = net.Record(1);
      self.self_answer = true;
      answer(std::move(self));
    } else {
      DhtLookupAnswer other;
      other.record = net.Record(9);
      answer(std::move(other));
    }
  }
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->record.has_value());
  EXPECT_EQ(outcome->record->seq, 1) << "the subject's own answer";
  EXPECT_LE(max_inflight, DhtLookup::Limits{}.alpha);
}

// Publishing walks to the k closest even when someone already holds a record for the key.
TEST(DhtLookupTest, AWalkGoesOnPastARecordAndNamesTheClosestThatAnswered) {
  Network net(200, 61, 20);
  const DhtLookup::Limits walk = DhtLookup::Limits::WalkToClosest();
  const auto outcome = RunInline(net, FarSeeds(net), walk);
  ASSERT_EQ(outcome.closest.size(), walk.k) << "asked " << outcome.asked << " answered " << outcome.answered;
  std::vector<std::string> ids;
  for (const auto& [id, node] : net.nodes) {
    ids.push_back(id);
  }
  const DhtKey key = *DhtKeyOfPeerId(net.target);
  std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
    return DhtCloser(key, *DhtKeyOfPeerId(a), *DhtKeyOfPeerId(b));
  });
  ids.resize(walk.k);
  EXPECT_EQ(outcome.closest, ids) << "exactly the network's k closest, closest first (asked " << outcome.asked << ")";
  EXPECT_LE(outcome.asked, walk.max_asks);
}

TEST(DhtLookupTest, ASmallerKWalksToFewer) {
  Network net(200, 61, 4);
  const DhtLookup::Limits walk = DhtLookup::Limits::WalkToClosest(4);
  EXPECT_EQ(walk.max_asks, DhtLookup::Limits{}.max_asks) << "never fewer asks than a lookup";
  const auto outcome = RunInline(net, FarSeeds(net), walk);
  ASSERT_EQ(outcome.closest.size(), 4u);
  const DhtKey key = *DhtKeyOfPeerId(net.target);
  for (const auto& [id, node] : net.nodes) {
    if (std::find(outcome.closest.begin(), outcome.closest.end(), id) == outcome.closest.end()) {
      EXPECT_FALSE(DhtCloser(key, *DhtKeyOfPeerId(id), *DhtKeyOfPeerId(outcome.closest.back())))
          << id << " is closer than the 4th found";
    }
  }
}

TEST(DhtLookupTest, TheClosestListHasOnlyThoseThatAnswered) {
  Network net(60, 11, 20);
  const auto seeds = FarSeeds(net);
  const std::string silent = seeds[1].peer_key;
  std::optional<DhtLookup::Outcome> outcome;
  DhtLookup::Limits limits;
  limits.follow_hints = false;
  DhtLookup::Run(
      net.target, seeds, limits,
      [&](const DhtLookupCandidate& c, DhtLookup::AnswerCb answer) {
        if (c.peer_key == silent) {
          answer(std::nullopt);
        } else {
          answer(DhtLookupAnswer{});
        }
      },
      {}, [&](DhtLookup::Outcome o) { outcome = std::move(o); });
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->closest.size(), 2u);
  EXPECT_EQ(std::count(outcome->closest.begin(), outcome->closest.end(), silent), 0);
}

} // namespace
} // namespace pp::mesh
