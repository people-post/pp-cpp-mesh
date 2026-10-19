// Long-lived product legs must not be reset for idling (pp-cpp-amp v2.15.0+ enforces
// ChannelPolicy::read_timeout: nothing inbound for that long resets the channel). Each of
// these legs is legitimately silent on one side for long stretches; their coordinators own
// handshake deadlines and liveness instead.

#include "domain/mesh/l4/circuit/CircuitChannelPolicy.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

constexpr std::chrono::milliseconds kNever{0};

// Call control: a standby path heartbeats every 10 s; the call must keep it.
TEST(ProductChannelPoliciesTest, CallControlLegNeverIdleResets) {
  EXPECT_EQ(pp::amp::CallMediaControlChannelPolicy().read_timeout, kNever);
  EXPECT_EQ(pp::amp::CallMediaChannelPolicy().read_timeout, kNever);
}

// Media relay: a publisher receives nothing back on its attach leg (fan-out skips the sender).
TEST(ProductChannelPoliciesTest, MediaRelayLegsNeverIdleReset) {
  EXPECT_EQ(pp::amp::MediaRelayClientChannelPolicy().read_timeout, kNever);
  EXPECT_EQ(pp::amp::MediaRelayHopChannelPolicy().read_timeout, kNever);
}

// Circuit: reservations wait silently for their TTL; spliced tunnels carry one-way media.
TEST(ProductChannelPoliciesTest, CircuitLegsNeverIdleReset) {
  EXPECT_EQ(pp::amp::CircuitTunnelChannelPolicy().read_timeout, kNever);
  EXPECT_EQ(pbr::CircuitTargetChannelPolicy(pp::amp::kAmpCircuitCarrierProtocolId).read_timeout, kNever);
  EXPECT_EQ(pbr::CircuitTargetChannelPolicy("/pp-browser/media-relay/1.0.0").read_timeout, kNever);
}

} // namespace
