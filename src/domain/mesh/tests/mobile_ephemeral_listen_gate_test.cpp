#include "domain/mesh/reachability/MobileEphemeralListenGate.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

MobileEphemeralListenInput EligibleInput() {
  MobileEphemeralListenInput in;
  in.is_mobile = true;
  in.messaging_ready = true;
  in.node_runtime_running = true;
  in.on_wifi = true;
  in.foreground = true;
  in.active_local_call = true;
  in.ephemeral_active = false;
  return in;
}

TEST(MobileEphemeralListenGateTest, StartsWhenEligible) {
  EXPECT_TRUE(ShouldStartMobileEphemeralListen(EligibleInput()));
}

TEST(MobileEphemeralListenGateTest, StartsDuringForegroundIncomingRing) {
  auto in = EligibleInput();
  in.active_local_call = true;
  EXPECT_TRUE(ShouldStartMobileEphemeralListen(in));
}

TEST(MobileEphemeralListenGateTest, DoesNotStartWhenAlreadyActive) {
  auto in = EligibleInput();
  in.ephemeral_active = true;
  EXPECT_FALSE(ShouldStartMobileEphemeralListen(in));
}

TEST(MobileEphemeralListenGateTest, DoesNotStartOffWifi) {
  auto in = EligibleInput();
  in.on_wifi = false;
  EXPECT_FALSE(ShouldStartMobileEphemeralListen(in));
}

TEST(MobileEphemeralListenGateTest, StopsWhenBackground) {
  auto in = EligibleInput();
  in.ephemeral_active = true;
  in.foreground = false;
  EXPECT_TRUE(ShouldStopMobileEphemeralListen(in));
}

TEST(MobileEphemeralListenGateTest, StopsWhenCallEnds) {
  auto in = EligibleInput();
  in.ephemeral_active = true;
  in.active_local_call = false;
  EXPECT_TRUE(ShouldStopMobileEphemeralListen(in));
}

TEST(MobileEphemeralListenGateTest, DoesNotStopWhenInactive) {
  auto in = EligibleInput();
  in.foreground = false;
  EXPECT_FALSE(ShouldStopMobileEphemeralListen(in));
}

} // namespace
} // namespace pbr
