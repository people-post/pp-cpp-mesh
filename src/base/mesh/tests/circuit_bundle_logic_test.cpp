#include "base/mesh/l4/circuit/CircuitBundleLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CircuitBundleLogicTest, AdmitAllowsEmptyContacts) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "bridge";
  ctx.dialer_peer_id = "peer-a";
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::Allow);
}

TEST(CircuitBundleLogicTest, AdmitRefusesStranger) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "bridge";
  ctx.dialer_peer_id = "stranger";
  ctx.contact_peer_ids = {"friend"};
  ctx.serve_scope_mask = kRelayScopeLinkSiteSocial;
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::RefuseStranger);
}

TEST(CircuitBundleLogicTest, AdmitRefusesBadOpAndNotReady) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "quote";
  ctx.dialer_peer_id = "peer-a";
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::RefuseBadOp);

  ctx.op = "bridge";
  ctx.service_started = false;
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::RefuseNotReady);
}

TEST(CircuitBundleLogicTest, AckDecisions) {
  EXPECT_EQ(DecideCircuitBridgeAck({.phase = CircuitTunnelPhase::WaitAck, .ack_ok = true}),
            CircuitBridgeAckDecision::EnterBridging);
  EXPECT_EQ(DecideCircuitBridgeAck({.phase = CircuitTunnelPhase::WaitAck, .ack_ok = false}),
            CircuitBridgeAckDecision::Fail);
  EXPECT_EQ(DecideCircuitBridgeAck({.phase = CircuitTunnelPhase::Bridging, .ack_ok = true}),
            CircuitBridgeAckDecision::IgnoreStale);
}

TEST(CircuitBundleLogicTest, CloseDecisions) {
  EXPECT_EQ(DecideCircuitTunnelClose({.phase = CircuitTunnelPhase::Bridging, .local_cancel = true}),
            CircuitTunnelCloseDecision::SuppressNotify);
  EXPECT_EQ(DecideCircuitTunnelClose({.phase = CircuitTunnelPhase::WaitAck, .remote_terminal = true}),
            CircuitTunnelCloseDecision::FailTunnel);
  EXPECT_EQ(DecideCircuitTunnelClose({.phase = CircuitTunnelPhase::Idle}), CircuitTunnelCloseDecision::Ignore);
  EXPECT_TRUE(CircuitTunnelPhaseIsActive(CircuitTunnelPhase::ServeDial));
  EXPECT_FALSE(CircuitTunnelPhaseIsActive(CircuitTunnelPhase::Closing));
}

} // namespace
} // namespace pbr
