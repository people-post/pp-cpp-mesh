#include "domain/mesh/l4/circuit/CircuitBundleLogic.h"

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

// K003: a full relay refuses standby circuits lowest priority first; primary circuits are never
// refused for standby load; one dialer holds at most its cap of standby circuits.
TEST(CircuitBundleLogicTest, StandbyAdmissionByPriorityAndPerDialerCap) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "bridge";
  ctx.dialer_peer_id = "peer-a";
  ctx.max_standby = 10;
  ctx.max_standby_per_dialer = 3;
  const auto admit = [&](CircuitStandbyPriority priority, size_t total, size_t mine) {
    ctx.standby_priority = priority;
    ctx.standby_total = total;
    ctx.standby_from_dialer = mine;
    return DecideCircuitAdmit(ctx);
  };
  EXPECT_EQ(admit(CircuitStandbyPriority::Low, 4, 0), CircuitAdmitDecision::Allow);
  EXPECT_EQ(admit(CircuitStandbyPriority::Low, 5, 0), CircuitAdmitDecision::RefuseStandbyFull) << "half full";
  EXPECT_EQ(admit(CircuitStandbyPriority::Medium, 7, 0), CircuitAdmitDecision::Allow);
  EXPECT_EQ(admit(CircuitStandbyPriority::Medium, 8, 0), CircuitAdmitDecision::RefuseStandbyFull) << "80 %";
  EXPECT_EQ(admit(CircuitStandbyPriority::High, 9, 0), CircuitAdmitDecision::Allow);
  EXPECT_EQ(admit(CircuitStandbyPriority::High, 10, 0), CircuitAdmitDecision::RefuseStandbyFull);
  EXPECT_EQ(admit(CircuitStandbyPriority::High, 0, 3), CircuitAdmitDecision::RefuseStandbyFull) << "per-dialer cap";
  EXPECT_EQ(admit(CircuitStandbyPriority::None, 10, 3), CircuitAdmitDecision::Allow) << "a primary circuit";
  ctx.op = "reserve";
  EXPECT_EQ(admit(CircuitStandbyPriority::Low, 10, 3), CircuitAdmitDecision::Allow) << "reservations are not standby";
}

TEST(CircuitBundleLogicTest, StandbyPriorityWire) {
  for (auto p : {CircuitStandbyPriority::Low, CircuitStandbyPriority::Medium, CircuitStandbyPriority::High}) {
    EXPECT_EQ(ParseCircuitStandbyPriority(CircuitStandbyPriorityWire(p)), p);
  }
  EXPECT_EQ(CircuitStandbyPriorityWire(CircuitStandbyPriority::None), nullptr) << "field omitted";
  EXPECT_EQ(ParseCircuitStandbyPriority(""), CircuitStandbyPriority::None);
  EXPECT_EQ(ParseCircuitStandbyPriority("urgent"), CircuitStandbyPriority::Low) << "newer value: refused first";
}

TEST(CircuitBundleLogicTest, AdmitAllowsReserveOp) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "reserve";
  ctx.dialer_peer_id = "peer-a";
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::Allow);
}

TEST(CircuitBundleLogicTest, AdmitAllowsStrangerWhenPublicScope) {
  CircuitAdmitContext ctx;
  ctx.service_started = true;
  ctx.op = "bridge";
  ctx.dialer_peer_id = "stranger";
  ctx.contact_peer_ids = {"friend"};
  ctx.serve_scope_mask =
      kRelayScopeShortTerm | static_cast<RelayScopeMask>(RelayScope::Public);
  EXPECT_EQ(DecideCircuitAdmit(ctx), CircuitAdmitDecision::Allow);
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
