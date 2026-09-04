#include "domain/mesh/reachability/AmpPunchCoordinator.h"

#include "domain/mesh/tests/support/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AmpPunchCoordinatorTest, NotStartedReturnsCodedFailure) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  AmpPunchCoordinator punch(harness->mgr_a(), {}, {});
  auto punched = punch.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::NotStarted);
}

TEST(AmpPunchCoordinatorTest, StartedRequiresIntroducerEndpoint) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  auto pump = [&]() { harness->PumpBoth(); };
  AmpPunchCoordinator punch(harness->mgr_a(), pump, {});
  punch.Start();
  auto punched = punch.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::EndpointNotRegistered);
  punch.Stop();
}

TEST(AmpPunchCoordinatorTest, StubRejectsReadyColdPunchUntilOrchestrationLands) {
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("introducer", harness->ma_b)));
  auto pump = [&]() { harness->PumpBoth(); };
  AmpPunchCoordinator punch(harness->mgr_a(), pump, {});
  punch.SetLocalCandidateAddrs({harness->ma_a});
  punch.Start();
  auto punched = punch.TryColdPunch("introducer", harness->peer_id_b, {harness->ma_a}, 1000);
  ASSERT_FALSE(static_cast<bool>(punched));
  EXPECT_EQ(punched.error().GetCode(), AmpPunchCoordinator::Err::PunchFailed);
  punch.Stop();
}

} // namespace
} // namespace pbr
