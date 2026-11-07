#include "mesh/types/ServiceLoad.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pp::mesh {
namespace {

// The level follows the pressure with a gap between on and off, so a node near
// a line does not flap.
TEST(ServiceLoadTest, ALevelTurnsOnAtItsLineAndOffBelowIt) {
  ServiceLoad level = ServiceLoad::Open;
  level = NextServiceLoad(level, 0.65, false);
  EXPECT_EQ(level, ServiceLoad::Open) << "below busy's line";
  level = NextServiceLoad(level, 0.72, false);
  EXPECT_EQ(level, ServiceLoad::Busy);
  level = NextServiceLoad(level, 0.65, false);
  EXPECT_EQ(level, ServiceLoad::Busy) << "between the lines: stays";
  level = NextServiceLoad(level, 0.96, false);
  EXPECT_EQ(level, ServiceLoad::Full);
  level = NextServiceLoad(level, 0.92, false);
  EXPECT_EQ(level, ServiceLoad::Full) << "between the lines: stays";
  level = NextServiceLoad(level, 0.85, false);
  EXPECT_EQ(level, ServiceLoad::Busy);
  level = NextServiceLoad(level, 0.10, false);
  EXPECT_EQ(level, ServiceLoad::Open);
  EXPECT_EQ(NextServiceLoad(ServiceLoad::Open, 0.0, true),
            ServiceLoad::Draining)
      << "draining whatever the pressure";
}

// The worst source sets a service's level; draining sets every service's, and
// lifting it returns them.
TEST(ServiceLoadTest, TheWorstSourceSetsTheLevelAndDrainingSetsAll) {
  ServiceLoadMonitor monitor;
  monitor.SetPressure(MeshService::Park, "reservations", 0.20);
  monitor.SetPressure(MeshService::Park, "links", 0.97);
  EXPECT_EQ(monitor.Level(MeshService::Park), ServiceLoad::Full);
  EXPECT_EQ(monitor.Level(MeshService::Relay), ServiceLoad::Open)
      << "its own pressure";
  monitor.SetPressure(MeshService::Park, "links", 0.30);
  EXPECT_EQ(monitor.Level(MeshService::Park), ServiceLoad::Open);
  monitor.SetDraining(true);
  for (const MeshService service :
       {MeshService::Park, MeshService::Relay, MeshService::MediaRelay}) {
    EXPECT_EQ(monitor.Level(service), ServiceLoad::Draining)
        << MeshServiceName(service);
  }
  monitor.SetDraining(false);
  EXPECT_EQ(monitor.Level(MeshService::Park), ServiceLoad::Open);
  monitor.SetPressure(MeshService::MediaRelay, "cpu", 7.0);
  EXPECT_DOUBLE_EQ(monitor.Pressure(MeshService::MediaRelay), 1.0) << "clamped";
}

// The wire words round-trip; a word from a newer node reads Unknown.
TEST(ServiceLoadTest, TheWireWordsRoundTrip) {
  for (const ServiceLoad load : {ServiceLoad::Open, ServiceLoad::Busy,
                                 ServiceLoad::Full, ServiceLoad::Draining}) {
    EXPECT_EQ(ServiceLoadFromWire(ServiceLoadWire(load)), load);
  }
  EXPECT_EQ(ServiceLoadFromWire("overloaded"), ServiceLoad::Unknown);
  EXPECT_STREQ(ServiceLoadWire(ServiceLoad::Unknown), "");
}

// What a node said is good for a while; an older node's silence keeps it;
// candidates order best first.
TEST(ServiceLoadTest, TheBookKeepsWhatANodeSaidForAWhile) {
  PeerLoadBook book;
  book.Note("a", MeshService::Park, ServiceLoad::Full, 1000);
  EXPECT_TRUE(
      book.RefusesNew("a", MeshService::Park, 1000 + PeerLoadBook::kFreshMs));
  EXPECT_EQ(book.Load("a", MeshService::Relay, 1000), ServiceLoad::Unknown)
      << "per service";
  EXPECT_EQ(book.Load("a", MeshService::Park, 1001 + PeerLoadBook::kFreshMs),
            ServiceLoad::Unknown)
      << "stale";
  book.Note("a", MeshService::Park, ServiceLoad::Unknown, 2000);
  EXPECT_EQ(book.Load("a", MeshService::Park, 2000), ServiceLoad::Full)
      << "silence is not news";

  book.Note("busy", MeshService::Relay, ServiceLoad::Busy, 5000);
  book.Note("open", MeshService::Relay, ServiceLoad::Open, 5000);
  book.Note("full", MeshService::Relay, ServiceLoad::Full, 5000);
  std::vector<std::string> order = {"full", "busy", "unknown", "open"};
  book.OrderBest(order, MeshService::Relay, 5000);
  EXPECT_EQ(order,
            (std::vector<std::string>{"open", "unknown", "busy", "full"}));
}

// Kernel drops a second as a pressure: none is idle, kDropsPerSecondFull and more is full.
TEST(ServiceLoadTest, DropsBecomeAPressure) {
  EXPECT_DOUBLE_EQ(DropRatePressure(0), 0.0);
  EXPECT_NEAR(DropRatePressure(kDropsPerSecondFull * 0.5), 0.5, 1e-9);
  EXPECT_DOUBLE_EQ(DropRatePressure(kDropsPerSecondFull * 3), 1.0);
}

} // namespace
} // namespace pp::mesh
