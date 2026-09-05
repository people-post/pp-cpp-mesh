#pragma once

#include "amp/L3/ChannelSession.h"
#include "amp/link/MeshRuntime.h"
#include "domain/mesh/l4/circuit/CircuitBridgeTarget.h"
#include "domain/mesh/l4/circuit/CircuitBundleLogic.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

struct CircuitTunnelBridgeResult {
  bool ok = false;
  std::string error;
  std::string resolved_multiaddr;
  /** Client circuit session after bridge ack (coordinator-owned; valid while Bridging). */
  std::shared_ptr<pp::amp::ChannelSession> session;
};

/**
 * Non-blocking `/pp-browser/circuit/1.0.0` tunnels on MeshRuntime ([A022]).
 * MeshHost always Starts the coordinator when Amp is up (outbound client). Inbound hosting
 * is gated by SetServeInbound. SoftMigrate NAT adopts the bridged ChannelSession via
 * AmpCircuitHopRegistry ([A020] / D9 step 5c).
 * No IoPump / nested Pump; OpenChannel + PostToIo callbacks only.
 */
class CircuitTunnelCoordinator {
public:
  using FrameHandler = pp::amp::ChannelSession::FrameHandler;
  using ClosedCallback = pp::amp::ChannelSession::ClosedCallback;
  using BridgeFinished = std::function<void(Roe<CircuitTunnelBridgeResult>)>;

  explicit CircuitTunnelCoordinator(pp::amp::MeshRuntime& runtime);
  ~CircuitTunnelCoordinator();

  CircuitTunnelCoordinator(const CircuitTunnelCoordinator&) = delete;
  CircuitTunnelCoordinator& operator=(const CircuitTunnelCoordinator&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

  void SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy);

  /** When false, inbound bridges are refused (outbound StartBridge still works). */
  void SetServeInbound(bool serve);
  bool ServeInbound() const;

  /** Cancel all in-flight / bridging tunnels (Leave / shutdown). */
  void AbortInflight();

  /**
   * Client: returns tunnel id immediately; completion via `on_finished` when Bridging or error.
   * Optional `on_payload` receives forwarded DATA after ack.
   */
  CircuitTunnelId StartBridge(const std::string& relay_peer_key, const CircuitBridgeTarget& target,
                              FrameHandler on_payload = {}, ClosedCallback on_closed = {},
                              BridgeFinished on_finished = {}, int timeout_ms = 8000);

  void CancelTunnel(CircuitTunnelId id);

  CircuitTunnelPhase Phase(CircuitTunnelId id) const;
  bool IsTunnelActive(CircuitTunnelId id) const;

  /** Bridging client session (null if not ready / relay-serve). */
  std::shared_ptr<pp::amp::ChannelSession> Session(CircuitTunnelId id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
