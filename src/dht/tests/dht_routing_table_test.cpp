// Keys, distance and the routing table of the DHT (docs/DHT.md § 4.1–4.2).
#include "mesh/dht/DhtKey.h"
#include "mesh/dht/DhtRoutingTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pp::mesh {
namespace {

std::string Hex(const DhtKey& key) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : key) {
    out += digits[b >> 4];
    out += digits[b & 0xf];
  }
  return out;
}

/** A distinct base58 "PeerId" per index (any base58 text has a key). */
std::string Pid(int i) {
  static const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::string tail;
  do {
    tail += alphabet[static_cast<size_t>(i % 58)];
    i /= 58;
  } while (i > 0);
  return "QmTest" + tail;
}

std::string Ma(int a, int b, int c, int d) {
  return "/ip4/" + std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d) +
         "/udp/1/adp/1.0.0";
}

// The contract's key: BLAKE2b-256 over the domain and the PeerId's multihash bytes.
TEST(DhtKeyTest, KeyOfAPeerIdMatchesTheContract) {
  // Independent: Python hashlib.blake2b(b"pp-mesh/dht/peer-routing/v1\0" + base58decode(id), digest_size=32).
  EXPECT_EQ(Hex(*DhtKeyOfPeerId("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP")),
            "9fed9f77fb1dda22c3c37799476d29c4dc62ab175a8acec5319fd3ab07f3e867");
  EXPECT_EQ(Hex(*DhtKeyOfPeerId("QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie")),
            "47696f0f4fac9c5481b822db64e34e7b1c86964fd034be8e27c68f67a4f231b4");
  EXPECT_FALSE(DhtKeyOfPeerId("not base58: 0OIl"));
  EXPECT_FALSE(DhtKeyOfPeerId(""));
  EXPECT_EQ(*Base58Decode("111z"), (std::vector<uint8_t>{0, 0, 0, 0x39})) << "each leading 1 is a zero byte";
}

TEST(DhtKeyTest, BucketIndexAndCloseness) {
  DhtKey self{};
  DhtKey one{};
  one[31] = 1;
  DhtKey top{};
  top[0] = 0x80;
  EXPECT_EQ(DhtBucketIndex(self, self), -1);
  EXPECT_EQ(DhtBucketIndex(self, one), 0);
  EXPECT_EQ(DhtBucketIndex(self, top), 255);
  EXPECT_TRUE(DhtCloser(self, one, top));
  EXPECT_FALSE(DhtCloser(self, top, one));
  EXPECT_FALSE(DhtCloser(self, one, one));
}

/** Peers that fall in `self`'s bucket 255 (half of all keys do). */
std::vector<std::string> InTopBucket(const DhtRoutingTable& table, const size_t n) {
  std::vector<std::string> out;
  for (int i = 0; out.size() < n && i < 10000; ++i) {
    if (DhtBucketIndex(table.SelfKey(), *DhtKeyOfPeerId(Pid(i))) == 255) {
      out.push_back(Pid(i));
    }
  }
  return out;
}

TEST(DhtRoutingTableTest, AdmitsRefreshesAndRejectsItself) {
  DhtRoutingTable table("QmSeAfNode", 3);
  ASSERT_TRUE(table.Valid());
  EXPECT_EQ(table.NoteHeard("QmSeAfNode", Ma(1, 1, 1, 1), 0).heard, DhtRoutingTable::Heard::Rejected);
  EXPECT_EQ(table.NoteHeard("bad 0OIl", Ma(1, 1, 1, 1), 0).heard, DhtRoutingTable::Heard::Rejected);
  const auto peers = InTopBucket(table, 1);
  EXPECT_EQ(table.NoteHeard(peers[0], Ma(10, 0, 0, 1), 1).heard, DhtRoutingTable::Heard::Added);
  EXPECT_EQ(table.NoteHeard(peers[0], Ma(10, 0, 9, 1), 2).heard, DhtRoutingTable::Heard::Updated);
  EXPECT_EQ(table.Size(), 1u);
  EXPECT_EQ(table.Closest(table.SelfKey(), 1).front().multiaddr, Ma(10, 0, 9, 1));
}

// A full bucket keeps its long-lived entries: the newcomer waits, the oldest is to be pinged; when
// the oldest goes, the newest replacement moves in.
TEST(DhtRoutingTableTest, AFullBucketKeepsTheOldAndQueuesTheNew) {
  DhtRoutingTable table("QmSeAfNode", 3);
  const auto peers = InTopBucket(table, 5);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(table.NoteHeard(peers[i], Ma(10, i, 0, 1), i).heard, DhtRoutingTable::Heard::Added);
  }
  const auto pending = table.NoteHeard(peers[3], Ma(10, 3, 0, 1), 10);
  EXPECT_EQ(pending.heard, DhtRoutingTable::Heard::Pending);
  ASSERT_TRUE(pending.ping.has_value());
  EXPECT_EQ(pending.ping->peer_id, peers[0]) << "the oldest heard";
  EXPECT_FALSE(table.Contains(peers[3]));
  table.Remove(peers[0]);  // its ping went unanswered
  EXPECT_FALSE(table.Contains(peers[0]));
  EXPECT_TRUE(table.Contains(peers[3]));
  EXPECT_EQ(table.Size(), 3u);
}

// One subnet gets at most two places in a bucket — many keys behind one address cannot fill it.
TEST(DhtRoutingTableTest, ASubnetGetsAtMostTwoPlacesPerBucket) {
  DhtRoutingTable table("QmSeAfNode", 20);
  const auto peers = InTopBucket(table, 4);
  EXPECT_EQ(table.NoteHeard(peers[0], Ma(203, 0, 113, 1), 0).heard, DhtRoutingTable::Heard::Added);
  EXPECT_EQ(table.NoteHeard(peers[1], Ma(203, 0, 113, 2), 0).heard, DhtRoutingTable::Heard::Added);
  EXPECT_EQ(table.NoteHeard(peers[2], Ma(203, 0, 113, 3), 0).heard, DhtRoutingTable::Heard::Rejected);
  EXPECT_EQ(table.NoteHeard(peers[3], Ma(203, 0, 114, 3), 0).heard, DhtRoutingTable::Heard::Added);
}

TEST(DhtRoutingTableTest, ClosestIsSortedByDistance) {
  DhtRoutingTable table("QmSeAfNode", 20);
  for (int i = 0; i < 300; ++i) {
    (void)table.NoteHeard(Pid(i), Ma(10, i / 200, i % 200, 1), i);
  }
  const DhtKey target = *DhtKeyOfPeerId("QmSomeTarget");
  const auto closest = table.Closest(target, 8);
  ASSERT_EQ(closest.size(), 8u);
  for (size_t i = 1; i < closest.size(); ++i) {
    EXPECT_FALSE(DhtCloser(target, closest[i].key, closest[i - 1].key)) << "sorted, closest first";
  }
  // Nothing in the table is closer than the 8th.
  const auto all = table.Closest(target, 1000);
  EXPECT_EQ(all.front().peer_id, closest.front().peer_id);
  EXPECT_TRUE(table.StaleBuckets(0).empty()) << "nothing was heard before t=0";
  EXPECT_FALSE(table.StaleBuckets(1000).empty()) << "every bucket was last heard before t=1000";
}

TEST(DhtRoutingTableTest, ClosestLeavesOutTheAskerAndResetStartsOver) {
  DhtRoutingTable table;
  EXPECT_FALSE(table.Valid());
  EXPECT_EQ(table.NoteHeard(Pid(1), Ma(10, 0, 1, 1), 0).heard, DhtRoutingTable::Heard::Rejected) << "no self yet";
  table.Reset("QmSeAfNode");
  ASSERT_TRUE(table.Valid());
  for (int i = 0; i < 5; ++i) {
    (void)table.NoteHeard(Pid(i), Ma(10, 0, i, 1), i);
  }
  const DhtKey target = *DhtKeyOfPeerId(Pid(2));
  EXPECT_EQ(table.Closest(target, 1).front().peer_id, Pid(2));
  const auto without = table.Closest(target, 10, Pid(2));
  EXPECT_EQ(without.size(), 4u);
  EXPECT_TRUE(std::none_of(without.begin(), without.end(), [](const DhtContact& c) { return c.peer_id == Pid(2); }));
  ASSERT_TRUE(table.Find(Pid(3)).has_value());
  EXPECT_EQ(table.Find(Pid(3))->multiaddr, Ma(10, 0, 3, 1));

  table.Reset("QmSeAfNode");
  EXPECT_EQ(table.Size(), 5u) << "the same self: kept";
  table.Reset("QmAnotherSeAf");
  EXPECT_TRUE(table.Valid());
  EXPECT_EQ(table.Size(), 0u) << "another self: start over";
  table.Reset("QmAnotherSeAf", 4);
  EXPECT_EQ(table.K(), 4u);
  const auto peers = InTopBucket(table, 6);
  for (size_t i = 0; i < peers.size(); ++i) {
    (void)table.NoteHeard(peers[i], Ma(10, 1, static_cast<int>(i), 1), 0);
  }
  EXPECT_EQ(table.InBucket(255).size(), 4u) << "buckets of k = 4";
}

} // namespace
} // namespace pp::mesh
