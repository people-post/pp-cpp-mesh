#include "domain/mesh/l4/circuit/CircuitBundleLogic.h"

namespace pbr {

CircuitAdmitDecision DecideCircuitAdmit(const CircuitAdmitContext& ctx) {
  if (!ctx.service_started || ctx.stopping) {
    return CircuitAdmitDecision::RefuseNotReady;
  }
  if (ctx.op != "bridge" && ctx.op != "reserve") {
    return CircuitAdmitDecision::RefuseBadOp;
  }
  if (!RelayAdmissionAllowsDialer(ctx.serve_scope_mask, ctx.dialer_peer_id, ctx.contact_peer_ids)) {
    return CircuitAdmitDecision::RefuseStranger;
  }
  if (ctx.op == "bridge" && ctx.standby_priority != CircuitStandbyPriority::None) {
    if (ctx.standby_from_dialer >= ctx.max_standby_per_dialer) {
      return CircuitAdmitDecision::RefuseStandbyFull;
    }
    // Full relays refuse the least needed standby first (K003): stationary pairs on a direct path
    // below half the capacity, punched paths below 80 %, mobile / unknown pairs up to the cap.
    size_t limit = ctx.max_standby;
    if (ctx.standby_priority == CircuitStandbyPriority::Low) {
      limit = ctx.max_standby / 2;
    } else if (ctx.standby_priority == CircuitStandbyPriority::Medium) {
      limit = ctx.max_standby * 4 / 5;
    }
    if (ctx.standby_total >= limit) {
      return CircuitAdmitDecision::RefuseStandbyFull;
    }
  }
  return CircuitAdmitDecision::Allow;
}

CircuitBridgeAckDecision DecideCircuitBridgeAck(const CircuitBridgeAckContext& ctx) {
  if (ctx.phase != CircuitTunnelPhase::WaitAck) {
    return CircuitBridgeAckDecision::IgnoreStale;
  }
  if (!ctx.ack_ok) {
    return CircuitBridgeAckDecision::Fail;
  }
  return CircuitBridgeAckDecision::EnterBridging;
}

CircuitTunnelCloseDecision DecideCircuitTunnelClose(const CircuitTunnelCloseContext& ctx) {
  if (ctx.finished || ctx.phase == CircuitTunnelPhase::Idle || ctx.phase == CircuitTunnelPhase::Closing) {
    return CircuitTunnelCloseDecision::Ignore;
  }
  if (ctx.local_cancel) {
    return CircuitTunnelCloseDecision::SuppressNotify;
  }
  if (ctx.remote_terminal || CircuitTunnelPhaseIsActive(ctx.phase)) {
    return CircuitTunnelCloseDecision::FailTunnel;
  }
  return CircuitTunnelCloseDecision::Ignore;
}

bool CircuitTunnelPhaseIsActive(const CircuitTunnelPhase phase) {
  switch (phase) {
  case CircuitTunnelPhase::OutboundOpen:
  case CircuitTunnelPhase::WaitAck:
  case CircuitTunnelPhase::Reserved:
  case CircuitTunnelPhase::ServeDial:
  case CircuitTunnelPhase::Bridging:
    return true;
  case CircuitTunnelPhase::Idle:
  case CircuitTunnelPhase::Closing:
    return false;
  }
  return false;
}

} // namespace pbr
