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
 * Client side of `/pp-browser/circuit/1.0.0` on MeshRuntime ([A022]): ask a relay to bridge to a
 * target (`StartBridge`), or park on a relay so it can bridge to us without dialing into our NAT
 * (`StartReserve`). MeshHost Starts one whenever Amp is up. SoftMigrate NAT adopts the bridged
 * ChannelSession via AmpCircuitHopRegistry ([A020] / D9 step 5c).
 * No IoPump / nested Pump; OpenChannel + PostToIo callbacks only.
 */
class CircuitClientCoordinator {
public:
  using FrameHandler = pp::amp::ChannelSession::FrameHandler;
  using ClosedCallback = pp::amp::ChannelSession::ClosedCallback;
  using BridgeFinished = std::function<void(Roe<CircuitTunnelBridgeResult>)>;

  explicit CircuitClientCoordinator(pp::amp::MeshRuntime& runtime);
  ~CircuitClientCoordinator();

  CircuitClientCoordinator(const CircuitClientCoordinator&) = delete;
  CircuitClientCoordinator& operator=(const CircuitClientCoordinator&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const;

  /** Cancel all in-flight / bridging tunnels and reservations (Leave / shutdown). */
  void AbortInflight();

  /**
   * Returns tunnel id immediately; completion via `on_finished` when Bridging or error.
   * Optional `on_payload` receives forwarded DATA after ack.
   */
  CircuitTunnelId StartBridge(const std::string& relay_peer_key, const CircuitBridgeTarget& target,
                              FrameHandler on_payload = {}, ClosedCallback on_closed = {},
                              BridgeFinished on_finished = {}, int timeout_ms = 8000);

  /**
   * Park on relay until TTL / CancelTunnel so R can bridge to this PeerId without dialing into
   * NAT (answerer outbound Session).
   */
  CircuitTunnelId StartReserve(const std::string& relay_peer_key, BridgeFinished on_finished = {},
                               int timeout_ms = 30000);

  void CancelTunnel(CircuitTunnelId id);

  CircuitTunnelPhase Phase(CircuitTunnelId id) const;
  bool IsTunnelActive(CircuitTunnelId id) const;

  /** Bridging session (null if not ready). */
  std::shared_ptr<pp::amp::ChannelSession> Session(CircuitTunnelId id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
