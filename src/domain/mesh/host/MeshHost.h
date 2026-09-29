#pragma once

#include "foundation/data/Config.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/host/LocalNetworkChange.h"
#include "domain/mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "domain/mesh/dht/AmpDhtProtocol.h"
#include "domain/mesh/discovery/AmpDirectoryProtocol.h"
#include "domain/mesh/reachability/dial_back/client/DialBackClient.h"
#include "domain/mesh/reachability/dial_back/serve/DialBackServer.h"
#include "domain/mesh/reachability/punch/AmpPunchCoordinator.h"
#include "domain/mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "domain/mesh/l4/media_relay/serve/MediaRelayServer.h"
#include "domain/mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "domain/mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "domain/mesh/host/MeshIdentityConfig.h"
#include "domain/mesh/host/MeshPorts.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/mesh/host/MeshPumpThread.h"
#include "domain/mesh/reachability/ReachabilityEngine.h"
#include "common/Error.h"

#include <chrono>
#include <atomic>
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
  /** Participate in mesh DHT (Node with dht capability). */
  bool host_dht = false;
  /** Serve Amp directory twin (pp-node / org seed; N029 nd4). */
  bool host_directory = false;
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

  bool MeshPumpRunning() const { return pump_.IsRunning(); }

  ReachabilityEngine& Reachability();

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
  /**
   * Ranked dialable Amp listen MAs for ch0 / DHT / invite (global /ip6 before private /ip4).
   * Empty when Amp is down. May expand wildcard binds via LAN + GlobalIpv6Addresses.
   */
  std::vector<std::string> AdvertisedListenMultiaddrs() const;
  /** Set when Amp was requested but failed (Start returns error; for diagnostics). */
  const std::string& AmpLastError() const { return amp_last_error_; }

  /** circuit serving side (bridge / reserve for others); gated by host_circuit_relay. */
  CircuitRelayServer* AmpCircuitServer();
  /** circuit client side (our bridges and reservations on relays). */
  CircuitClientCoordinator* AmpCircuitClient();
  /** media_relay serving side (inbound quote / attach, hosted sessions); gated by host_media_relay. */
  MediaRelayServer* AmpMediaRelayServer();
  /** media_relay client side (outbound quote / attach, the attached session). */
  MediaRelayClientCoordinator* AmpMediaRelayClientCoord();
  AmpCircuitHopRegistry* AmpCircuitHops();
  /** Amp dial-back for reachability chrome (D8); null when Amp is down. */
  DialBackClient* AmpDialBack();
  /** Amp coordinated punch (H009 / L3.25a); null when Amp is down. */
  AmpPunchCoordinator* AmpPunch();
  /** Amp mesh DHT (n2); null when Amp is down. */
  AmpDhtProtocol* AmpDht();
  /** Amp directory twin (N029 nd4); null when Amp is down. */
  AmpDirectoryProtocol* AmpDirectory();

  void ConfigureAmpDht(AmpDhtProtocolConfig config);
  /** Hot refresh: advertisement + participate flag without restart. */
  void RefreshAmpDhtHosting(bool host_dht);

  void ConfigureAmpDirectory(AmpDirectoryProtocolConfig config);
  /** Hot refresh: advertise `/pp-mesh/directory/1.0.0` when serving. */
  void RefreshAmpDirectoryHosting(bool host_directory);

  /** Who drives the attached stack's Amp turn loop. */
  enum class AttachDrive {
    /** Caller Ticks (VirtualClock / MemoryDatagramIo tests — MeshPump is not clock-safe). */
    Manual,
    /** Own MeshPump and hand L4 work to AppRuntime workers like product Start (wall-clock harnesses, e.g. pp-call-probe). */
    MeshPump,
  };
  Roe<void> AttachAmpStack(std::unique_ptr<pp::amp::AmpStack> stack, std::string listen_multiaddr = {},
                           AttachDrive drive = AttachDrive::Manual);

  const std::string& LastError() const { return last_error_; }

  void AbortInflightCircuitRequests();

  /** Build deps and run Amp reachability probe (async). */
  void StartReachabilityProbe(bool try_upnp_first = false);
  /**
   * k5: the device's network changed (any thread). Per `DecideLocalNetworkReaction`: Amp probes
   * every direct link and evicts the silent ones fast; reachability is probed again once that
   * settled, refreshing advertised and punch addresses. A newer change supersedes a pending re-probe.
   */
  void OnLocalNetworkChanged(const LocalNetworkChange& change);
  void RunReachabilityProbeBlocking(bool try_upnp_first = false);

private:
  Roe<void> StartAmpFromConfig(const MeshHostConfig& config);
  void StopAmp();
  void ApplyAmpAdvertisement(const MeshHostConfig& config);
  void EnsureAmpL4Coordinators();
  void RefreshAdvertisedListenAddrs();
  /** When `refresh_listen_addrs` is false, keep caller-supplied listen multiaddrs (AttachAmpStack /
   * MemoryDatagramIo tests) instead of expanding from real LAN NICs. */
  void StartAmpL4Hosting(bool host_circuit, bool host_media, bool host_dht, bool host_directory,
                         bool refresh_listen_addrs = true);
  AmpReachabilityProbeDeps MakeReachabilityDeps(bool try_upnp_first) const;
  void StartOwnedThreads();
  void StopOwnedThreads();
  /** Empty under MeshPump; AttachAmpStack harnesses return Tick for sync AmpParkUntil. */
  std::function<void()> MakeL4IoPump() const;
  std::function<void(std::function<void()>)> MakeL4IoPost() const;
  /** MeshRuntime::PostAfter — Amp-clock delayed work (deadlines). */
  std::function<void(std::chrono::milliseconds, std::function<void()>)> MakeL4IoAfter() const;
  /**
   * Where L4 inbound handlers do their CPU / disk work off the IO strand: AppRuntime workers once
   * this host drives its own MeshPump (product, wall-clock harnesses); inline on the driving thread
   * for manual-drive harnesses (no runtime). Nothing posted here may wait on the mesh.
   */
  std::function<void(std::function<void()>)> MakeL4WorkerPost(WorkerLane lane) const;

  std::unique_ptr<ReachabilityEngine> reachability_;
  MeshPumpThread pump_;
  /** Set while MeshPump drives: L4 work goes to AppRuntime workers (MakeL4WorkerPost). */
  std::atomic<bool> l4_on_workers_{false};
  /** k5: the latest network change; a delayed re-probe for an older one stands down. */
  std::atomic<uint64_t> network_change_gen_{0};
  void ReprobeAfterNetworkChange(uint64_t gen, int attempts_left);
  std::unique_ptr<pp::amp::AmpStack> amp_;
  std::unique_ptr<AmpCircuitHopRegistry> amp_circuit_hops_;
  std::unique_ptr<CircuitRelayServer> amp_circuit_server_;
  std::unique_ptr<CircuitClientCoordinator> amp_circuit_client_;
  std::unique_ptr<MediaRelayServer> amp_media_relay_server_;
  /** Holds a pointer to the server (local hop): declared after it, so freed first. */
  std::unique_ptr<MediaRelayClientCoordinator> amp_media_relay_client_;
  std::unique_ptr<DialBackServer> amp_dial_back_server_;
  std::unique_ptr<DialBackClient> amp_dial_back_;
  std::unique_ptr<AmpPunchCoordinator> amp_punch_;
  std::unique_ptr<AmpDhtProtocol> amp_dht_;
  std::unique_ptr<AmpDirectoryProtocol> amp_directory_;
  bool host_dht_ = false;
  bool host_directory_ = false;
  /** True for product Start (MeshPump); false for AttachAmpStack harnesses. */
  bool prefer_mesh_pump_ = false;
  std::unique_ptr<IChatPeerLinks> chat_links_;
  std::shared_ptr<pp::adp::Clock> amp_clock_;
  std::string amp_listen_multiaddr_;
  std::string amp_last_error_;
  std::string last_error_;
  std::vector<std::string> bootstrap_peers_;
};

} // namespace pbr
