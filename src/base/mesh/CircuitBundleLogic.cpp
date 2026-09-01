#include "base/mesh/CircuitBundleLogic.h"

namespace pbr {

CircuitAdmitDecision DecideCircuitAdmit(const CircuitAdmitContext& ctx) {
  if (!ctx.service_started || ctx.stopping) {
    return CircuitAdmitDecision::RefuseNotReady;
  }
  if (ctx.op != "bridge") {
    return CircuitAdmitDecision::RefuseBadOp;
  }
  if (!RelayAdmissionAllowsDialer(ctx.serve_scope_mask, ctx.dialer_peer_id, ctx.contact_peer_ids)) {
    return CircuitAdmitDecision::RefuseStranger;
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
