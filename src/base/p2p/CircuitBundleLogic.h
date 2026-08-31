#pragma once

#include "base/people/RelayScope.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace pbr {

struct CircuitTunnelId {
  uint64_t value = 0;
  explicit operator bool() const { return value != 0; }
  friend bool operator==(CircuitTunnelId a, CircuitTunnelId b) { return a.value == b.value; }
};

/** AMP-native circuit tunnel phase (not a libp2p stream bridge phase). */
enum class CircuitTunnelPhase {
  Idle = 0,
  /** Client: opening circuit channel to relay. */
  OutboundOpen,
  /** Client: bridge JSON sent; waiting for ack. */
  WaitAck,
  /** Relay: dialing / opening target protocol. */
  ServeDial,
  /** Relay: ack sent; ChannelBridge armed (or client: ack ok, splice live). */
  Bridging,
  Closing,
};

enum class CircuitTunnelRole {
  Client = 0,
  RelayServe,
};

enum class CircuitAdmitDecision {
  Allow = 0,
  RefuseStranger,
  RefuseNotReady,
  RefuseBadOp,
};

struct CircuitAdmitContext {
  bool service_started = false;
  bool stopping = false;
  std::string dialer_peer_id;
  std::string op;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
};

CircuitAdmitDecision DecideCircuitAdmit(const CircuitAdmitContext& ctx);

enum class CircuitBridgeAckDecision {
  EnterBridging = 0,
  Fail,
  IgnoreStale,
};

struct CircuitBridgeAckContext {
  CircuitTunnelPhase phase = CircuitTunnelPhase::Idle;
  bool ack_ok = false;
};

CircuitBridgeAckDecision DecideCircuitBridgeAck(const CircuitBridgeAckContext& ctx);

enum class CircuitTunnelCloseDecision {
  Ignore = 0,
  FailTunnel,
  SuppressNotify,
};

struct CircuitTunnelCloseContext {
  CircuitTunnelPhase phase = CircuitTunnelPhase::Idle;
  bool local_cancel = false;
  bool remote_terminal = false;
  bool finished = false;
};

CircuitTunnelCloseDecision DecideCircuitTunnelClose(const CircuitTunnelCloseContext& ctx);

bool CircuitTunnelPhaseIsActive(CircuitTunnelPhase phase);

} // namespace pbr
