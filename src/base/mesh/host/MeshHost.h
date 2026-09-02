#pragma once

#include "base/data/Config.h"
#include "amp/link/AmpStack.h"
#include "base/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "base/mesh/reachability/AmpDialBackService.h"
#include "base/mesh/l4/media_relay/AmpMediaRelayCoordinator.h"
#include "base/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "base/mesh/host/MeshIdentityConfig.h"
#include "base/mesh/host/MeshPorts.h"
#include "base/mesh/reachability/ReachabilityService.h"
#include "common/Error.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Amp-only mesh host (A017/D10): owns AmpStack + Amp L4 coordinators + reachability.
 *
 * Product SoftMigrate uses Amp when coordinators are up; circuit NAT adopts bridged
 * ChannelSessions via AmpCircuitHopRegistry. Dial-back (D8) feeds Me→Network chrome.
 */
struct MeshHostConfig {
  /** ML-DSA identity for Amp PeerId (device keys; historically shared with Libp2pHost). */
  MeshIdentityConfig host;
  /** Start inbound circuit-relay hosting (Node with circuit_relay capability). */
  bool host_circuit_relay = false;
  /** Start inbound media_relay hosting (Node with media_relay capability). */
  bool host_media_relay = false;
  MediaRelayBudgetConfig media_relay_budget{};
  RelayPricingConfig media_relay_pricing{};
  /** Fire an Amp dial-back reachability probe after start (Node / pp-node). */
  bool start_reachability_probe = false;
  bool try_upnp_first = false;
  /** Optional probe-completion callback (worker thread). */
  std::function<void()> on_reachability_updated;
  /** Bootstrap peers for seed dial (ADP multiaddrs preferred for dial-back). */
  std::vector<std::string> bootstrap_peers;

  /**
   * Peer mesh on/off. When true, bind UDP + AmpStack (D10 hard-require).
   * Requires `host.device_ml_dsa_*` so AMP PeerId matches identity.
   * Failure fails `MeshHost::Start` (no TCP underlay fallback).
   */
  bool mesh_enabled = false;
  /** ADP UDP listen port; 0 = ephemeral. */
  uint16_t amp_udp_port = 0;
};

class MeshHost {
public:
  MeshHost();
  ~MeshHost();

  MeshHost(const MeshHost&) = delete;
  MeshHost& operator=(const MeshHost&) = delete;

  Roe<void> Start(const MeshHostConfig& config);
  void Stop();
  void Tick();
  bool IsRunning() const;

  ReachabilityService& Reachability();

  /** Feature-layer chat port bundle (null when Amp is down). */
  std::optional<MeshChatDeps> ChatDeps();
  /** Circuit + hop registry + links for feature reach helpers. */
  std::optional<MeshCircuitDeps> CircuitDeps();

  /**
   * Low-level Amp access — mesh tests and AttachAmpStack only.
   * Feature code must use ChatDeps() / CircuitDeps() instead.
   */
  pp::amp::AmpStack* Amp();
  const pp::amp::AmpStack* Amp() const;
  const std::string& AmpListenMultiaddr() const { return amp_listen_multiaddr_; }
  /** Set when Amp was requested but failed (Start returns error; for diagnostics). */
  const std::string& AmpLastError() const { return amp_last_error_; }

  CircuitTunnelCoordinator* AmpCircuitTunnel();
  AmpMediaRelayCoordinator* AmpMediaRelayCoord();
  AmpCircuitHopRegistry* AmpCircuitHops();
  /** Amp dial-back for reachability chrome (D8); null when Amp is down. */
  AmpDialBackService* AmpDialBack();

  Roe<void> AttachAmpStack(std::unique_ptr<pp::amp::AmpStack> stack, std::string listen_multiaddr = {});

  const std::string& LastError() const { return last_error_; }

  void AbortInflightCircuitRequests();

  /** Build deps and run Amp reachability probe (async). */
  void StartReachabilityProbe(bool try_upnp_first = false);
  void RunReachabilityProbeBlocking(bool try_upnp_first = false);

private:
  Roe<void> StartAmpFromConfig(const MeshHostConfig& config);
  void StopAmp();
  void ApplyAmpAdvertisement(const MeshHostConfig& config);
  void EnsureAmpL4Coordinators();
  void StartAmpL4Hosting(bool host_circuit, bool host_media);
  AmpReachabilityProbeDeps MakeReachabilityDeps(bool try_upnp_first) const;

  std::unique_ptr<ReachabilityService> reachability_;
  std::unique_ptr<pp::amp::AmpStack> amp_;
  std::unique_ptr<AmpCircuitHopRegistry> amp_circuit_hops_;
  std::unique_ptr<CircuitTunnelCoordinator> amp_circuit_;
  std::unique_ptr<AmpMediaRelayCoordinator> amp_media_relay_;
  std::unique_ptr<AmpDialBackService> amp_dial_back_;
  std::unique_ptr<IChatPeerLinks> chat_links_;
  std::shared_ptr<pp::adp::Clock> amp_clock_;
  std::string amp_listen_multiaddr_;
  std::string amp_last_error_;
  std::string last_error_;
  std::vector<std::string> bootstrap_peers_;
};

} // namespace pbr
