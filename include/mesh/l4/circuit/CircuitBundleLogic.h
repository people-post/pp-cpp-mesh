#pragma once

#include "mesh/l4/circuit/CircuitBridgeTarget.h"
#include "mesh/types/RelayScope.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_set>

namespace pp::mesh {

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
  /** Client: bridge or reserve JSON sent; waiting for ack. */
  WaitAck,
  /** Client: reserve ack ok; holding circuit channel until cancelled / TTL. */
  Reserved,
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

/** Relay-wide standby circuits, and per dialer PeerId (K009 per-account cap, device-scoped). */
inline constexpr size_t kCircuitDefaultMaxStandby = 64;
inline constexpr size_t kCircuitDefaultMaxStandbyPerDialer = 4;

/**
 * What one relay holds for others (PB-022): live bridges relay-wide and from one dialer PeerId, and
 * parked reservations (one per PeerId — PeerIds cost nothing to make). Far above a device's need
 * (a call's bridge or two, a standby, a probe); past them a request is refused Busy.
 */
inline constexpr size_t kCircuitMaxTunnels = 4096;
inline constexpr size_t kCircuitMaxTunnelsPerDialer = 32;
inline constexpr size_t kCircuitMaxReservations = 8192;
/** A dialer's timeout_ms is its wish, bounded: bridge setup, and how long a reservation is held. */
inline constexpr int64_t kCircuitDefaultBridgeTimeoutMs = 8'000;
inline constexpr int64_t kCircuitMaxBridgeTimeoutMs = 30'000;
inline constexpr int64_t kCircuitDefaultReserveTimeoutMs = 30'000;
inline constexpr int64_t kCircuitMaxReserveTimeoutMs = 60'000;
/** New target addresses one dialer may make this relay dial, per minute (PB-023). */
inline constexpr size_t kCircuitMaxNewTargetsPerDialerPerMinute = 16;

/** The requested timeout: the default when absent or 0, at most `max`. */
int64_t CircuitRequestTimeoutMs(std::optional<int64_t> requested, int64_t fallback, int64_t max);

/**
 * Protocols a bridge may open on its target (PB-020): the far leg opens with the relay's identity,
 * so a dialer may reach only what a circuit is for — the call-media carrier, a media relay behind a
 * circuit, a nested circuit — and lab probes' `/test/…` protocols (no product handler serves one).
 */
bool CircuitTargetProtocolAllowed(std::string_view protocol);

enum class CircuitAdmitDecision {
  Allow = 0,
  RefuseStranger,
  RefuseNotReady,
  RefuseBadOp,
  /** K003: standby capacity (for this priority) or the dialer's standby cap is used up. */
  RefuseStandbyFull,
};

struct CircuitAdmitContext {
  bool service_started = false;
  bool stopping = false;
  std::string dialer_peer_id;
  std::string op;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
  std::unordered_set<std::string> contact_peer_ids;
  /** K003 standby admission: this request's priority and the relay's live standby circuits. */
  CircuitStandbyPriority standby_priority = CircuitStandbyPriority::None;
  size_t standby_total = 0;
  size_t standby_from_dialer = 0;
  size_t max_standby = kCircuitDefaultMaxStandby;
  size_t max_standby_per_dialer = kCircuitDefaultMaxStandbyPerDialer;
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

} // namespace pp::mesh
