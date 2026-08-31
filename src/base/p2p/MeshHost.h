#pragma once

#include "base/data/Config.h"
#include "base/mesh/link/AmpStack.h"
#include "common/Error.h"
#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/DialBackService.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/MediaRelayService.h"
#include "base/p2p/NodeRuntime.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/ReachabilityService.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Shared libp2p mesh host (Wave 1): NodeRuntime + dial-back + circuit / media relay +
 * reachability, converging the pp-browser MessagingHub and headless pp-node start paths.
 *
 * Optional parallel AMP stack ([A020](../../projects/adp/DECISIONS.md)): when enabled or
 * attached, owns `amp::AmpStack` but does not yet route product L4 traffic.
 *
 * NOT owned here (app-only glue): CallMediaDirectService, LanMdnsDiscovery.
 */
struct MeshHostConfig {
  NodeRuntimeConfig runtime;
  /** Start inbound circuit-relay hosting (Node with circuit_relay capability). */
  bool host_circuit_relay = false;
  /** Start inbound media_relay hosting (Node with media_relay capability). */
  bool host_media_relay = false;
  MediaRelayBudgetConfig media_relay_budget{};
  RelayPricingConfig media_relay_pricing{};
  /** Fire a reachability probe after start (Node / pp-node). */
  bool start_reachability_probe = false;
  bool try_upnp_first = false;
  /** Optional probe-completion callback (worker thread). */
  std::function<void()> on_reachability_updated;

  /**
   * Bind UDP + AmpStack beside libp2p (no L4 traffic flip).
   * Requires `runtime.host.device_ml_dsa_*` so AMP PeerId matches libp2p.
   */
  bool enable_amp_stack = false;
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

  NodeRuntime* Runtime();
  Libp2pHost* Host();
  PeerSessionManager* Sessions() const;
  DialBackService* DialBack();
  CircuitRelayService* CircuitRelay();
  MediaRelayService* MediaRelay();
  ReachabilityService& Reachability();

  /** Parallel AMP stack when `enable_amp_stack` or `AttachAmpStack` (may be null). */
  amp::AmpStack* Amp();
  const amp::AmpStack* Amp() const;
  const std::string& AmpListenMultiaddr() const { return amp_listen_multiaddr_; }

  /**
   * Install a parallel AmpStack (tests / custom DatagramIo). Takes ownership, starts the
   * stack, and clears any previous AmpStack. Does not flip product L4 traffic.
   */
  Roe<void> AttachAmpStack(std::unique_ptr<amp::AmpStack> stack, std::string listen_multiaddr = {});

  const std::string& BoundListenMultiaddr() const;
  const std::string& LastError() const { return last_error_; }

  /** Abort in-flight circuit RequestBridge waiters (shutdown / Leave paths). */
  void AbortInflightCircuitRequests();

private:
  Roe<void> StartAmpFromConfig(const MeshHostConfig& config);
  void StopAmp();
  void ApplyAmpAdvertisement(const MeshHostConfig& config);

  std::unique_ptr<NodeRuntime> runtime_;
  std::unique_ptr<DialBackService> dial_back_;
  std::unique_ptr<CircuitRelayService> circuit_relay_;
  std::unique_ptr<MediaRelayService> media_relay_;
  std::unique_ptr<ReachabilityService> reachability_;
  std::unique_ptr<amp::AmpStack> amp_;
  std::shared_ptr<adp::Clock> amp_clock_;
  std::string amp_listen_multiaddr_;
  std::string last_error_;
};

} // namespace pbr
