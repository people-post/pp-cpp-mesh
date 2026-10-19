#include "domain/mesh/reach/CircuitRendezvousCoordinator.h"
#include "domain/mesh/reach/PunchIntroducerWalk.h"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

// No mesh (before start / after stop): the punch step answers at once instead of hanging reach.
TEST(PunchIntroducerWalkTest, WithoutAMeshPunchesFailAtOnce) {
  PunchIntroducerWalk walk;
  std::optional<Roe<void>> cold;
  walk.TryColdPunchAsync("12D3KooWTarget", [&](Roe<void> r) { cold = std::move(r); });
  ASSERT_TRUE(cold.has_value());
  EXPECT_FALSE(*cold);
  std::optional<Roe<void>> upgrade;
  walk.TryUpgradePunchAsync("12D3KooWIntro", "12D3KooWTarget", [&](Roe<void> r) { upgrade = std::move(r); });
  ASSERT_TRUE(upgrade.has_value());
  EXPECT_FALSE(*upgrade);
}

// No mesh: no dial surface, and a park request answers `false` (H010 callers must not wait forever).
TEST(CircuitRendezvousCoordinatorTest, WithoutAMeshNothingIsDialableAndParkingAnswersFalse) {
  CircuitRendezvousCoordinator rendezvous;
  CircuitRendezvousDeps deps;
  deps.rendezvous_candidates = []() {
    MeshHopCandidate hop;
    hop.peer_id = "12D3KooWSeed";
    return std::vector<MeshHopCandidate>{hop};
  };
  rendezvous.SetDeps(std::move(deps));
  EXPECT_TRUE(rendezvous.DialableRelayIds("").empty());
  std::optional<bool> parked;
  rendezvous.EnsureBootstrapSeedParkedAsync([&](bool ok) { parked = ok; }, 50);
  ASSERT_TRUE(parked.has_value());
  EXPECT_FALSE(*parked);
}

} // namespace
} // namespace pbr
