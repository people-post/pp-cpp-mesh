#include "mesh/reachability/ReachabilityNetIf.h"
#include "mesh/reachability/AmpObservedAddrs.h"
#include "mesh/host/MeshHost.h"

#include "mesh/host/AmpLinkConfig.h"
#include "mesh/host/MeshLinkEventLog.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "mesh/reachability/dial_back/DialBackTypes.h"
#include "mesh/reachability/punch/PunchTypes.h"
#include <array>

#include "mesh/host/MeshPorts.h"

#include <utility>

#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L2/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "crypto/PeerId.h"
#include "common/Logger.h"
namespace pp::mesh {

namespace {

logging::Logger& MeshHostLog() {
  static logging::Logger logger = logging::getLogger("MeshHost");
  return logger;
}

} // namespace

MeshHost::MeshHost(MeshExecutors executors, MetricsRegistry& metrics)
    : reachability_(std::make_unique<ReachabilityEngine>()), executors_(std::move(executors)), metrics_(metrics) {}

MeshHost::~MeshHost() { Stop(); }

Roe<void> MeshHost::Start(const MeshHostConfig& config) {
  Stop();
  last_error_.clear();
  amp_last_error_.clear();

  // D10/A017: Amp is the only product underlay. mesh_enabled=false leaves mesh off.
  if (!config.mesh_enabled) {
    reachability_ = std::make_unique<ReachabilityEngine>();
    if (config.on_reachability_updated) {
      reachability_->SetOnUpdated(config.on_reachability_updated);
    }
    last_error_ = "mesh host: mesh_enabled required (TCP Host underlay retired)";
    return Error(last_error_);
  }

  if (auto amp_started = StartAmpFromConfig(config); !amp_started) {
    amp_last_error_ = amp_started.error().message;
    last_error_ = amp_last_error_;
    return amp_started.error();
  }

  bootstrap_peers_ = config.bootstrap_peers;
  reachability_ = std::make_unique<ReachabilityEngine>();
  // B26: always refresh ch0 / punch candidates when dial-back or UPnP lands, then notify product.
  // Runs on the io lane (the probe's): the link / punch updates belong there too, after it.
  reachability_->SetOnUpdated([this, product_cb = config.on_reachability_updated]() {
    MakeL4IoPost()([this]() { RefreshAdvertisedListenAddrs(); });
    if (product_cb) {
      product_cb();
    }
  });
  if (config.start_reachability_probe) {
    StartReachabilityProbe(config.try_upnp_first);
  }
  return {};
}

Roe<void> MeshHost::StartAmpFromConfig(const MeshHostConfig& config) {
  const auto& host_cfg = config.host;
  if (!host_cfg.device_ml_dsa_private_key || !host_cfg.device_ml_dsa_public_key) {
    return Error("mesh host: mesh_enabled requires device ML-DSA keys");
  }

  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key.assign(host_cfg.device_ml_dsa_private_key->begin(),
                                    host_cfg.device_ml_dsa_private_key->end());
  identity.ml_dsa_public_key.assign(host_cfg.device_ml_dsa_public_key->begin(),
                                    host_cfg.device_ml_dsa_public_key->end());

  auto peer_id = pp::PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  // Always dual-stack :: (call-path-resilience K010): the socket then serves IPv4 and IPv6 for the
  // whole run, so moving from an IPv4-only network to IPv6-only cellular (NAT64) needs no rebind.
  // pp-cpp-amp clears IPV6_V6ONLY and maps IPv4 peers both ways; advertise / probe targets come
  // from the interfaces, not the bind family. IPv4 only when the OS has no IPv6 socket support.
  // A host others connect to (seed / relay / DHT / directory) needs a link per client, and buffers for its bursts.
  const bool serves_others =
      config.host_circuit_relay || config.host_media_relay || config.host_dht || config.host_directory;
  pp::adp::UdpSocketOptions socket_options;
  if (serves_others) {
    socket_options.receive_buffer_bytes = kServerUdpBufferBytes;
    socket_options.send_buffer_bytes = kServerUdpBufferBytes;
  }
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V6({}, config.amp_udp_port), socket_options);
  if (!bound) {
    bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(0, 0, 0, 0, config.amp_udp_port), socket_options);
  }
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  io_ = io;
  amp_clock_ = std::make_shared<pp::adp::WallClock>();

  pp::amp::AmpStack::Config amp_cfg;
  amp_cfg.identity = std::move(identity);
  amp_cfg.local_peer_id = *peer_id;
  amp_cfg.link_config =
      MakeProductAmpLinkConfig(serves_others ? AmpLinkTable::Server : AmpLinkTable::Device, config.capacity.links);
  max_links_ = amp_cfg.link_config.max_links;
  const size_t accepted_connections = AcceptedConnectionsFor(amp_cfg.link_config);

  auto stack = pp::amp::AmpStack::Create(std::move(io), amp_clock_, std::move(amp_cfg));
  if (!stack) {
    return stack.error();
  }

  auto listen = pp::amp::FormatAdpMultiaddr((*stack)->LocalEndpoint(), *peer_id);
  if (!listen) {
    return listen.error();
  }

  (*stack)->Start();
  // Amp UDP accept is always on (Clients need inbound Amp for dial-back / LAN).
  (*stack)->GetEndpoint().SetAcceptEnabled(true);
  (*stack)->GetEndpoint().SetMaxAcceptedConnections(accepted_connections);
  amp_listen_multiaddr_ = *listen;
  (*stack)->Links().SetLocalListenMultiaddrs({amp_listen_multiaddr_});
  // Every protocol this host serves registers a handler: refuse opens for anything else, so a peer
  // asking for a service we do not run (e.g. broadcast admission on a plain relay / pp-node) fails
  // at once instead of waiting out its request timeout.
  (*stack)->Runtime().SetRefuseUnhandledOpens(true);
  amp_ = std::move(*stack);
  ApplyAddressDisclosureToAmp();
  InstallMeshLinkEventLog(amp_->Runtime());
  InstallMeshLinkMetrics(amp_->Runtime(), metrics_);
  chat_links_ = NewAmpChatPeerLinks(amp_->Runtime());
  ApplyAmpAdvertisement(config);
  prefer_mesh_pump_ = true;
  EnsureAmpL4Coordinators();
  host_dht_ = config.host_dht;
  host_directory_ = config.host_directory;
  StartAmpL4Hosting(config.host_circuit_relay, config.host_media_relay, config.host_dht,
                    config.host_directory);
  if (amp_media_relay_server_) {
    amp_media_relay_server_->SetVideoPolicy(config.media_relay_video);
    amp_media_relay_server_->SetMaxSessions(config.capacity.group_calls);
  }
  if (amp_circuit_server_) {
    amp_circuit_server_->SetCapacity(config.capacity.reservations, config.capacity.bridges);
  }
  StartOwnedThreads();
  return Roe<void>();
}

void MeshHost::StartOwnedThreads() {
  if (!amp_) {
    return;
  }
  l4_on_workers_.store(true, std::memory_order_release);
  if (!pump_.IsRunning()) {
    // The pump is the runtime's only driver: a drive from any other thread is refused (and counted).
    pump_.Start([this]() { return PumpTick(); }, MeshPumpThread::kDefaultInterval,
                [this]() { amp_->Runtime().ClaimDriver(); },
                [this]() { amp_->Runtime().ReleaseDriver(); });
  }
}

void MeshHost::StopOwnedThreads() {
  pump_.Stop();
  l4_on_workers_.store(false, std::memory_order_release);
}

std::function<void(std::function<void()>)> MeshHost::MakeL4WorkerPost(std::shared_ptr<pp::Executor> executor) const {
  return [this, executor = std::move(executor)](std::function<void()> task) {
    PostOrRun(l4_on_workers_.load(std::memory_order_acquire) ? executor : nullptr, std::move(task));
  };
}

std::shared_ptr<pp::SerialExecutor> MeshHost::IoLane() const {
  return amp_ ? amp_->Runtime().IoExecutor() : nullptr;
}

std::function<void()> MeshHost::MakeL4IoPump() const {
  // Exclusive Amp Drive: MeshPump product path never Ticks from L4 — AmpParkUntil sleeps
  // while MeshPumpThread Drives. AttachAmpStack harnesses have no MeshPump; sync Try*
  // AmpParkUntil on the harness thread (sole driver) must Tick here. Never invoke this
  // from PostToIo / mux (nested Drive is refused).
  if (prefer_mesh_pump_) {
    return {};
  }
  return [self = const_cast<MeshHost*>(this)]() { self->Tick(); };
}

std::function<void(std::function<void()>)> MeshHost::MakeL4IoPost() const {
  return [self = const_cast<MeshHost*>(this)](std::function<void()> task) {
    if (self->amp_ && task) {
      self->amp_->Runtime().PostToIo(std::move(task));
    }
  };
}

std::function<void(std::chrono::milliseconds, std::function<void()>)> MeshHost::MakeL4IoAfter() const {
  return [self = const_cast<MeshHost*>(this)](std::chrono::milliseconds delay,
                                              std::function<void()> task) {
    if (self->amp_ && task) {
      self->amp_->Runtime().PostAfter(delay, std::move(task));
    }
  };
}

void MeshHost::EnsureAmpL4Coordinators() {
  if (!amp_) {
    return;
  }
  if (!amp_circuit_hops_) {
    amp_circuit_hops_ = std::make_unique<AmpCircuitHopRegistry>();
  }
  if (!amp_circuit_server_) {
    amp_circuit_server_ = std::make_unique<CircuitRelayServer>(amp_->Runtime(), metrics_);
  }
  if (!amp_circuit_client_) {
    amp_circuit_client_ = std::make_unique<CircuitClientCoordinator>(amp_->Runtime());
  }
  if (!amp_media_relay_server_) {
    amp_media_relay_server_ = std::make_unique<MediaRelayServer>(amp_->Runtime(), metrics_);
  }
  if (!amp_media_relay_client_) {
    amp_media_relay_client_ =
        std::make_unique<MediaRelayClientCoordinator>(amp_->Runtime(), amp_media_relay_server_.get());
  }
  amp_media_relay_client_->SetCircuitHopRegistry(amp_circuit_hops_.get());
  amp_circuit_server_->SetLoadMonitor(&own_load_);
  amp_media_relay_server_->SetLoadMonitor(&own_load_);
  amp_circuit_client_->SetPeerLoadBook(&peer_loads_);
  amp_media_relay_client_->SetPeerLoadBook(&peer_loads_);
  if (load_tick_ == 0 && max_links_ > 0) {
    // Every service needs a link per client: the table's fill is a pressure on
    // each (sampled once a second). The levels for the operator too:
    // pp_service_load{service} (1 open, 2 busy, 3 full, 4 draining) and the
    // worst pressure behind it (pp-browser docs/contracts/NODE_METRICS.md).
    struct Gauges {
      MetricGauge *level = nullptr;
      MetricGauge *pressure = nullptr;
    };
    auto gauges = std::make_shared<std::array<Gauges, kMeshServiceCount>>();
    for (const MeshService service :
         {MeshService::Park, MeshService::Relay, MeshService::MediaRelay}) {
      const MetricLabels labels = {{"service", MeshServiceName(service)}};
      (*gauges)[static_cast<size_t>(service)] = {
          &metrics_.Gauge("pp_service_load",
                          "This node's level per service: 1 open, 2 busy, 3 "
                          "full, 4 draining",
                          labels),
          &metrics_.Gauge("pp_service_pressure",
                          "The worst pressure behind the level (0..1)",
                          labels)};
    }
    // The pump's busy share and the socket's kernel drops: what saturates a node before any count does (pp-browser
    // stress lab, 2026-11-06 — a node at 30 s of delay still counted itself open).
    MetricGauge* io_busy =
        &metrics_.Gauge("pp_io_busy_fraction", "Share of the last second the mesh pump worked (0..1)");
    MetricGauge* udp_drops =
        &metrics_.Gauge("pp_udp_receive_drops", "Datagrams the kernel dropped for want of receive buffer, since start");
    MetricGauge* udp_rcvbuf = &metrics_.Gauge("pp_udp_receive_buffer_bytes", "The socket's granted receive buffer");
    struct Sample {
      int64_t at_ms = 0;
      uint64_t drops = 0;
      double drop_pressure = 0.0;  // smoothed: one burst is not a full node
    };
    auto sample = std::make_shared<Sample>();
    load_tick_ = amp_->Runtime().AddIoTick([this, sample, gauges, io_busy, udp_drops, udp_rcvbuf]() {
      const int64_t now = PeerLoadBook::NowMs();
      if (now - sample->at_ms < 1000 || !amp_) {
        return;
      }
      const double seconds = sample->at_ms == 0 ? 1.0 : static_cast<double>(now - sample->at_ms) / 1000.0;
      sample->at_ms = now;
      const double links = static_cast<double>(amp_->Runtime().Links().CountLinks()) / static_cast<double>(max_links_);
      const double busy = pump_.IsRunning() ? pump_.BusyFraction() : 0.0;
      io_busy->Set(busy);
      if (const auto stats = io_ ? io_->Stats() : std::nullopt) {
        udp_rcvbuf->Set(static_cast<double>(stats->receive_buffer_bytes));
        if (stats->receive_drops) {
          const uint64_t drops = *stats->receive_drops;
          const double rate = drops >= sample->drops ? static_cast<double>(drops - sample->drops) / seconds : 0.0;
          sample->drops = drops;
          sample->drop_pressure = 0.5 * sample->drop_pressure + 0.5 * DropRatePressure(rate);
          udp_drops->Set(static_cast<double>(drops));
        }
      }
      for (const MeshService service : {MeshService::Park, MeshService::Relay, MeshService::MediaRelay}) {
        own_load_.SetPressure(service, "links", links);
        own_load_.SetPressure(service, "io", busy);
        own_load_.SetPressure(service, "udp_drops", sample->drop_pressure);
        const Gauges& g = (*gauges)[static_cast<size_t>(service)];
        g.level->Set(static_cast<double>(own_load_.Level(service)));
        g.pressure->Set(own_load_.Pressure(service));
      }
    });
  }
  auto io_pump = MakeL4IoPump();
  auto post_worker = MakeL4WorkerPost(executors_.work);
  if (!amp_dial_back_server_) {
    amp_dial_back_server_ = std::make_unique<DialBackServer>(amp_->Runtime());
  }
  if (!amp_dial_back_) {
    amp_dial_back_ = std::make_unique<DialBackClient>(amp_->Runtime(), io_pump);
  }
  if (!amp_punch_) {
    amp_punch_ = std::make_unique<AmpPunchCoordinator>(amp_->Runtime(), io_pump, metrics_);
    amp_punch_->SetAddressDisclosure(address_disclosure_);
  }
  if (!amp_dht_) {
    amp_dht_ = std::make_unique<AmpDhtProtocol>(amp_->Runtime(), post_worker);
    // Self-publish when due, on the lane with the rest of the DHT's state.
    dht_tick_ = amp_->Runtime().AddIoTick([dht = amp_dht_.get()]() { dht->Tick(); });
  }
  if (!amp_directory_) {
    amp_directory_ = std::make_unique<AmpDirectoryProtocol>(amp_->Runtime(), io_pump, post_worker);
  }
}

void MeshHost::StartAmpL4Hosting(const bool host_circuit, const bool host_media, const bool host_dht,
                                 const bool host_directory, const bool refresh_listen_addrs) {
  EnsureAmpL4Coordinators();
  host_dht_ = host_dht;
  host_directory_ = host_directory;
  // Always accept nested Session carriers so NAT call-media (A024) works without hosting circuit.
  if (amp_) {
    amp_->Links().EnableNestedCarrierAccept(true);
  }
  // Always Start so SoftMigrate guests / circuit clients can dial; inbound hosting is gated.
  if (amp_circuit_server_ && !amp_circuit_server_->IsStarted()) {
    amp_circuit_server_->Start();
  }
  if (amp_circuit_client_ && !amp_circuit_client_->IsStarted()) {
    amp_circuit_client_->Start();
  }
  if (amp_media_relay_server_ && !amp_media_relay_server_->IsStarted()) {
    amp_media_relay_server_->Start();
  }
  if (amp_media_relay_client_ && !amp_media_relay_client_->IsStarted()) {
    amp_media_relay_client_->Start();
  }
  if (amp_dial_back_server_ && !amp_dial_back_server_->IsStarted()) {
    amp_dial_back_server_->Start();
  }
  if (amp_dial_back_ && !amp_dial_back_->IsStarted()) {
    amp_dial_back_->Start();
  }
  if (amp_punch_ && !amp_punch_->IsStarted()) {
    amp_punch_->Start();
  }
  if (refresh_listen_addrs) {
    RefreshAdvertisedListenAddrs();
  }
  if (amp_dht_ && !amp_dht_->IsStarted()) {
    RunOnLane([this]() { amp_dht_->Start(); });
  }
  if (amp_directory_ && !amp_directory_->IsStarted()) {
    amp_directory_->Start();
  }
  if (amp_circuit_server_) {
    amp_circuit_server_->SetServeInbound(host_circuit);
  }
  if (amp_media_relay_server_) {
    amp_media_relay_server_->SetServeInbound(host_media);
  }
  ApplyAmpAdvertisement(MeshHostConfig{.host_circuit_relay = host_circuit,
                                       .host_media_relay = host_media,
                                       .host_dht = host_dht,
                                       .host_directory = host_directory});
}

void MeshHost::StopAmp() {
  prefer_mesh_pump_ = false;
  if (amp_) {
    amp_->Links().EnableNestedCarrierAccept(false);
  }
  // 1. Stop L4 while MeshPump still drives: completions they post during Stop still run.
  if (amp_punch_) {
    amp_punch_->Stop();
  }
  if (amp_dial_back_) {
    amp_dial_back_->Stop();
  }
  if (amp_dial_back_server_) {
    amp_dial_back_server_->Stop();
  }
  if (amp_directory_) {
    amp_directory_->Stop();
  }
  if (amp_dht_) {
    amp_dht_->Stop();
  }
  if (amp_media_relay_client_) {
    amp_media_relay_client_->Stop();
  }
  if (amp_media_relay_server_) {
    amp_media_relay_server_->Stop();
  }
  if (amp_circuit_client_) {
    amp_circuit_client_->Stop();
  }
  if (amp_circuit_server_) {
    amp_circuit_server_->Stop();
  }
  if (amp_circuit_hops_) {
    amp_circuit_hops_->ClearAll();
  }
  // 2. Join MeshPump (its Tick reads the L4 objects), 3. then free them, while Amp is still alive.
  StopOwnedThreads();
  amp_punch_.reset();
  amp_dial_back_.reset();
  amp_dial_back_server_.reset();
  amp_directory_.reset();
  if (amp_ && dht_tick_ != 0) {
    amp_->Runtime().RemoveIoTick(std::exchange(dht_tick_, 0));  // nothing drives now: MeshPump joined
  }
  if (amp_ && load_tick_ != 0) {
    amp_->Runtime().RemoveIoTick(std::exchange(load_tick_, 0));
  }
  amp_dht_.reset();
  amp_media_relay_client_.reset();
  amp_media_relay_server_.reset();
  amp_circuit_client_.reset();
  amp_circuit_server_.reset();
  amp_circuit_hops_.reset();
  if (amp_) {
    amp_->Stop();
    amp_.reset();
    io_.reset();
    blocked_listener_amp_ = nullptr;  // its listener went with it
  }
  chat_links_.reset();
  amp_clock_.reset();
  amp_listen_multiaddr_.clear();
  amp_last_error_.clear();
}

Roe<void> MeshHost::AttachAmpStack(std::unique_ptr<pp::amp::AmpStack> stack, std::string listen_multiaddr,
                                   const AttachDrive drive) {
  if (!stack) {
    return Error("mesh host: null AmpStack");
  }
  StopAmp();
  // Set before EnsureAmpL4Coordinators: L4 captures MakeL4IoPump (empty under MeshPump).
  prefer_mesh_pump_ = drive == AttachDrive::MeshPump;
  amp_ = std::move(stack);
  ApplyAddressDisclosureToAmp();
  amp_->Start();
  amp_listen_multiaddr_ = std::move(listen_multiaddr);
  if (!amp_listen_multiaddr_.empty()) {
    amp_->Links().SetLocalListenMultiaddrs({amp_listen_multiaddr_});
  }
  chat_links_ = NewAmpChatPeerLinks(amp_->Runtime());
  EnsureAmpL4Coordinators();
  // Tests / AttachAmpStack: start outbound-capable L4 without inbound hosting unless configured.
  // Keep the caller-supplied listen multiaddr — LAN refresh would replace MemoryDatagramIo
  // synthetic addrs (e.g. 10.0.0.1) with real NIC IPs.
  // Manual: no MeshPump — harnesses call Tick() (VirtualClock is not pump-safe).
  StartAmpL4Hosting(false, false, false, false, /*refresh_listen_addrs=*/false);
  InstallMeshLinkMetrics(amp_->Runtime(), metrics_);
  if (drive == AttachDrive::MeshPump) {
    InstallMeshLinkEventLog(amp_->Runtime());
    StartOwnedThreads();
    return Roe<void>();
  }
  return Roe<void>();
}

void MeshHost::ApplyAmpAdvertisement(const MeshHostConfig& config) {
  if (!amp_) {
    return;
  }
  // The app's protocols (SetAppProtocols), then the mesh's own.
  std::vector<std::string> protocols = AppProtocols();
  protocols.push_back(kDialBackProtocolId);
  protocols.push_back(kAmpPunchProtocolId);
  if (config.host_circuit_relay) {
    protocols.push_back(kCircuitRelayProtocolId);
  }
  if (config.host_media_relay) {
    protocols.push_back(kMediaRelayProtocolId);
  }
  if (config.host_dht) {
    protocols.push_back(kDhtProtocolId);
  }
  if (config.host_directory) {
    protocols.push_back(kDirectoryProtocolId);
  }
  amp_->Links().SetAdvertisedProtocols(std::move(protocols));
}

void MeshHost::Stop() {
  // Retire the probe first (its destructor waits for a step running on the lane), so no `on_updated`
  // runs while the Amp stack below goes away. Reachability() stays valid (a fresh engine).
  reachability_ = std::make_unique<ReachabilityEngine>();
  if (amp_circuit_client_) {
    amp_circuit_client_->AbortInflight();
  }
  if (amp_circuit_server_) {
    amp_circuit_server_->AbortInflight();
  }
  if (amp_media_relay_client_) {
    amp_media_relay_client_->AbortInflight();
  }
  if (amp_media_relay_server_) {
    amp_media_relay_server_->AbortInflight();
  }
  StopAmp();
  bootstrap_peers_.clear();
}

void MeshHost::Tick() {
  if (amp_) {
    // Sole Drive entry for this host. L4 must not call Tick to progress —
    // exclusive Amp Drive (docs/THREADING.md). Nested Drive is refused by MeshRuntime; under MeshPump a
    // drive from another thread is too. The DHT's self-publish is an io tick, inside the drive.
    amp_->Runtime().Drive();
  }
}

bool MeshHost::PumpTick() {
  if (!amp_) {
    return false;
  }
  const uint64_t before = amp_->Runtime().GetEndpoint().Stats().rx_datagrams;
  Tick();
  return amp_->Runtime().GetEndpoint().Stats().rx_datagrams - before >= pp::adp::kDefaultPumpBudget;
}

void MeshHost::RunOnLane(const std::function<void()>& task) {
  if (auto lane = IoLane()) {
    lane->RunAndWait(task);
  } else {
    task();
  }
}

bool MeshHost::IsRunning() const { return static_cast<bool>(amp_); }


std::vector<std::string> MeshHost::AdvertisedListenMultiaddrs() const {
  if (!amp_ || amp_listen_multiaddr_.empty()) {
    return {};
  }
  const auto observed =
      CollectAmpObservedAddrs(amp_listen_multiaddr_, amp_->LocalPeerId(), reachability_->Snapshot());
  auto merged = observed.MergedForAdvertise();
  if (merged.empty()) {
    merged.push_back(amp_listen_multiaddr_);
  }
  return merged;
}

void MeshHost::RefreshAdvertisedListenAddrs() {
  if (!amp_ || amp_listen_multiaddr_.empty()) {
    return;
  }
  const auto observed = CollectAmpObservedAddrs(amp_listen_multiaddr_, amp_->LocalPeerId(),
                                                reachability_->Snapshot());
  auto merged = observed.MergedForAdvertise();
  if (merged.empty()) {
    merged.push_back(amp_listen_multiaddr_);
  }
  amp_->Links().SetLocalListenMultiaddrs(std::move(merged));
  if (amp_punch_) {
    amp_punch_->SetLocalCandidateAddrs(observed.MergedForPunch());
  }
}

DialBackClient* MeshHost::AmpDialBack() { return amp_dial_back_.get(); }

AmpPunchCoordinator* MeshHost::AmpPunch() { return amp_punch_.get(); }

void MeshHost::SetAddressDisclosure(const AddressDisclosureGate* gate) {
  address_disclosure_ = gate;
  if (amp_punch_) {
    amp_punch_->SetAddressDisclosure(gate);
  }
  ApplyAddressDisclosureToAmp();
}

void MeshHost::ApplyAddressDisclosureToAmp() {
  if (!amp_) {
    return;
  }
  // ch0 capability: our listen multiaddrs go only to peers that may learn our address (privacy Y2).
  pp::amp::PeerLinkManager::ListenAddrDisclosure disclosure;
  if (const AddressDisclosureGate* gate = address_disclosure_) {
    disclosure = [gate](const std::string& remote_peer_id) { return gate->AllowsDirect(remote_peer_id); };
  }
  amp_->Links().SetListenAddrDisclosure(std::move(disclosure));
  if (blocked_listener_amp_ != amp_.get()) {
    blocked_listener_amp_ = amp_.get();
    // projects/privacy T3: a Blocked peer's link is dropped as soon as it connects (either direction).
    // The drop is scheduled (never inline on the event stack); the listener dies with this Amp stack.
    auto& runtime = amp_->Runtime();
    (void)runtime.AddLinkEventListener([this, &runtime](const pp::amp::LinkEvent& event) {
      const AddressDisclosureGate* gate = address_disclosure_;
      if (event.kind == pp::amp::LinkEvent::Kind::Connected && gate && gate->IsBlocked(event.peer_id)) {
        MeshHostLog().info << "dropping a Blocked peer's link peer=" << event.peer_id;
        (void)runtime.RequestDropLink(event.peer_id);
      }
    });
  }
}

AmpDhtProtocol* MeshHost::AmpDht() { return amp_dht_.get(); }

AmpDirectoryProtocol* MeshHost::AmpDirectory() { return amp_directory_.get(); }

void MeshHost::ConfigureAmpDht(AmpDhtProtocolConfig config) {
  if (!amp_dht_) {
    return;
  }
  // The publish gate and the published addresses follow the network (docs/DHT.md § 4.5).
  if (!config.current_listen_multiaddrs) {
    config.current_listen_multiaddrs = [this]() {
      auto addrs = AdvertisedListenMultiaddrs();
      std::erase_if(addrs, [](const std::string& ma) { return !IsUsableAdpListen(ma); });
      return addrs;
    };
  }
  if (!config.may_publish) {
    config.may_publish = [this]() { return reachability_->Snapshot().status != ReachabilityStatus::Blocked; };
  }
  RunOnLane([&]() { amp_dht_->Configure(std::move(config)); });  // its tick reads the config there
}

MeshHostConfig MeshHost::CurrentHosting() const {
  MeshHostConfig hosting;
  hosting.host_circuit_relay = amp_circuit_server_ && amp_circuit_server_->ServeInbound();
  hosting.host_media_relay = amp_media_relay_server_ && amp_media_relay_server_->ServeInbound();
  hosting.host_dht = host_dht_;
  hosting.host_directory = host_directory_;
  return hosting;
}

void MeshHost::SetAppProtocols(std::vector<std::string> protocol_ids) {
  {
    std::lock_guard lock(app_protocols_mu_);
    app_protocols_ = std::move(protocol_ids);
  }
  if (amp_) {
    ApplyAmpAdvertisement(CurrentHosting());
  }
}

std::vector<std::string> MeshHost::AppProtocols() const {
  std::lock_guard lock(app_protocols_mu_);
  return app_protocols_;
}

void MeshHost::RefreshAmpDhtHosting(const bool host_dht) {
  host_dht_ = host_dht;
  if (!amp_) {
    return;
  }
  ApplyAmpAdvertisement(CurrentHosting());
  if (amp_dht_ && host_dht) {
    RunOnLane([this]() { amp_dht_->Tick(); });
  }
}

void MeshHost::ConfigureAmpDirectory(AmpDirectoryProtocolConfig config) {
  if (!amp_directory_) {
    return;
  }
  amp_directory_->Configure(std::move(config));
}

void MeshHost::RefreshAmpDirectoryHosting(const bool host_directory) {
  host_directory_ = host_directory;
  if (!amp_) {
    return;
  }
  ApplyAmpAdvertisement(CurrentHosting());
}

AmpReachabilityProbeDeps MeshHost::MakeReachabilityDeps(bool try_upnp_first) const {
  AmpReachabilityProbeDeps deps;
  if (!amp_ || !amp_dial_back_) {
    return deps;
  }
  deps.links = &amp_->Links();
  deps.dial_back = amp_dial_back_.get();
  deps.amp_listen_multiaddr = amp_listen_multiaddr_;
  deps.local_peer_id = amp_->LocalPeerId();
  deps.bootstrap_peers = bootstrap_peers_;
  deps.io_pump = MakeL4IoPump();
  deps.post_worker = MakeL4WorkerPost(executors_.blocking);  // UPnP discovery blocks (~2 s)
  deps.post_io = MakeL4IoPost();
  deps.post_after = MakeL4IoAfter();
  deps.lane = IoLane();
  deps.try_upnp_first = try_upnp_first;
  return deps;
}

void MeshHost::StartReachabilityProbe(bool try_upnp_first) {
  if (!amp_ || !amp_dial_back_) {
    return;
  }
  reachability_->StartProbe(MakeReachabilityDeps(try_upnp_first));
}

void MeshHost::OnLocalNetworkChanged(const LocalNetworkChange& change) {
  const LocalNetworkReaction reaction = DecideLocalNetworkReaction(change);
  const uint64_t gen = network_change_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
  MeshHostLog().info << "local network changed online=" << (change.was_online ? 1 : 0) << "->" << (change.online ? 1 : 0)
             << " attachment_changed=" << (change.attachment_changed ? 1 : 0)
             << " probe_links=" << (reaction.probe_links ? 1 : 0);
  if (!amp_) {
    return;
  }
  if (reaction.probe_links) {
    amp_->Runtime().NotifyNetworkChanged();
    EnsureUdpSocketAlive(UdpSocketCheckReason::NetworkChange);  // #416: iOS can kill the socket on a path change
  }
  if (reaction.reprobe_reachability) {
    MakeL4IoAfter()(kReachabilityReprobeAfterNetworkChange, [this, gen]() { ReprobeAfterNetworkChange(gen, 5); });
  }
}

void MeshHost::ReprobeAfterNetworkChange(const uint64_t gen, const int attempts_left) {
  if (gen != network_change_gen_.load(std::memory_order_acquire) || !amp_) {
    return;  // superseded by a newer change (it schedules its own)
  }
  if (reachability_->IsProbing()) {
    // A probe from the old network is still out; its answer would be stale — probe again after it.
    if (attempts_left > 0) {
      MakeL4IoAfter()(std::chrono::seconds(1), [this, gen, attempts_left]() {
        ReprobeAfterNetworkChange(gen, attempts_left - 1);
      });
    }
    return;
  }
  StartReachabilityProbe(false);
}

void MeshHost::ProbeUdpSocket(std::function<void(const UdpSocketProbeResult&)> done) {
  if (!done) {
    return;
  }
  if (!amp_) {
    done(UdpSocketProbeResult{false, "no amp stack"});
    return;
  }
  MakeL4IoPost()([this, done = std::move(done)]() mutable { SendUdpSocketProbe(false, std::move(done)); });
}

void MeshHost::SendUdpSocketProbe(const bool second, std::function<void(const UdpSocketProbeResult&)> done) {
  if (!amp_) {
    done(UdpSocketProbeResult{false, "no amp stack"});
    return;
  }
  pp::adp::Endpoint& endpoint = amp_->GetEndpoint();
  const uint64_t rx_before = endpoint.Stats().rx_datagrams;
  const pp::adp::IpEndpoint target = UdpSocketProbeTarget(amp_->LocalEndpoint());
  static constexpr uint8_t kProbe[1] = {0};
  if (auto sent = endpoint.SendRaw(target, std::span<const uint8_t>(kProbe)); !sent) {
    MeshHostLog().warning << "udp socket probe: send failed: " << sent.error().message;
    done(UdpSocketProbeResult{false, sent.error().message});
    return;
  }
  const auto wait = second ? kUdpSocketProbeSecondWait : kUdpSocketProbeFirstWait;
  MakeL4IoAfter()(wait, [this, second, rx_before, done = std::move(done)]() mutable {
    if (!amp_) {
      done(UdpSocketProbeResult{false, "no amp stack"});
      return;
    }
    if (amp_->GetEndpoint().Stats().rx_datagrams > rx_before) {
      done(UdpSocketProbeResult{true, {}});
      return;
    }
    if (!second) {
      SendUdpSocketProbe(true, std::move(done));
      return;
    }
    MeshHostLog().warning << "udp socket probe: nothing received in "
                          << (kUdpSocketProbeFirstWait + kUdpSocketProbeSecondWait).count() << " ms";
    done(UdpSocketProbeResult{false, "nothing received"});
  });
}

void MeshHost::SetOnUdpSocketCheck(std::function<void(const UdpSocketCheck&)> on_check) {
  on_udp_socket_check_ = std::move(on_check);
}

void MeshHost::ReportUdpSocketCheck(const UdpSocketCheck& check) {
  udp_socket_check_running_.store(false, std::memory_order_release);
  if (on_udp_socket_check_) {
    on_udp_socket_check_(check);
  }
}

void MeshHost::EnsureUdpSocketAlive(const UdpSocketCheckReason reason) {
  if (!amp_ || udp_socket_check_running_.exchange(true, std::memory_order_acq_rel)) {
    return;  // no socket, or a check is running: its outcome covers this request
  }
  ProbeUdpSocket([this, reason](const UdpSocketProbeResult& probe) {
    UdpSocketCheck check;
    check.reason = reason;
    if (probe.ok) {
      check.was_alive = check.alive = true;
      ReportUdpSocketCheck(check);
      return;
    }
    check.error = probe.error;
    if (!amp_) {
      ReportUdpSocketCheck(check);
      return;
    }
    ReopenDeadUdpSocket(std::move(check));
  });
}

void MeshHost::ReopenDeadUdpSocket(UdpSocketCheck check) {
  // On the IO lane (the probe's answer): no send or receive runs beside the reopen.
  auto reopened = amp_->GetEndpoint().Io().Reopen();
  if (!reopened) {
    check.error += "; reopen failed: " + reopened.error().message;
    MeshHostLog().warning << "udp socket dead and not reopened: " << check.error;
    ReportUdpSocketCheck(check);
    return;
  }
  check.reopened = true;
  MeshHostLog().warning << "udp socket was dead (" << check.error << "); reopened on port "
                        << amp_->LocalEndpoint().port;
  // What a network change does: probe every link (silent ones go fast), learn our addresses again.
  amp_->Runtime().NotifyNetworkChanged();
  const uint64_t gen = network_change_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
  MakeL4IoAfter()(kReachabilityReprobeAfterNetworkChange, [this, gen]() { ReprobeAfterNetworkChange(gen, 5); });
  ProbeUdpSocket([this, check = std::move(check)](const UdpSocketProbeResult& after) mutable {
    check.alive = after.ok;
    if (!after.ok) {
      check.error += "; after reopen: " + after.error;
    }
    ReportUdpSocketCheck(check);
  });
}

void MeshHost::RunReachabilityProbeBlocking(bool try_upnp_first) {
  if (!amp_ || !amp_dial_back_) {
    return;
  }
  reachability_->RunProbeBlocking(MakeReachabilityDeps(try_upnp_first));
}

ReachabilityEngine& MeshHost::Reachability() { return *reachability_; }

std::optional<MeshChatDeps> MeshHost::ChatDeps() {
  if (!amp_ || !chat_links_) {
    return std::nullopt;
  }
  MeshIoContext io;
  io.io_pump = MakeL4IoPump();
  io.post_worker = MakeL4WorkerPost(executors_.work);
  io.post_io = MakeL4IoPost();
  io.post_after = MakeL4IoAfter();
  io.local_peer_id = amp_->LocalPeerId();
  // Keep raw bind here (hot path). Dialable advertise lives in AdvertisedListenMultiaddrs().
  io.listen_multiaddr = amp_listen_multiaddr_;
  return MeshChatDeps{std::move(io), *chat_links_};
}

std::optional<MeshCircuitDeps> MeshHost::CircuitDeps() {
  if (!amp_ || !chat_links_ || !amp_circuit_client_ || !amp_circuit_hops_) {
    return std::nullopt;
  }
  return MeshCircuitDeps{*amp_circuit_client_, *amp_circuit_hops_, *chat_links_};
}

pp::amp::AmpStack* MeshHost::Amp() { return amp_.get(); }

const pp::amp::AmpStack* MeshHost::Amp() const { return amp_.get(); }

CircuitRelayServer* MeshHost::AmpCircuitServer() { return amp_circuit_server_.get(); }
CircuitClientCoordinator* MeshHost::AmpCircuitClient() { return amp_circuit_client_.get(); }

MediaRelayServer* MeshHost::AmpMediaRelayServer() { return amp_media_relay_server_.get(); }
MediaRelayClientCoordinator* MeshHost::AmpMediaRelayClientCoord() { return amp_media_relay_client_.get(); }

AmpCircuitHopRegistry* MeshHost::AmpCircuitHops() { return amp_circuit_hops_.get(); }

void MeshHost::AbortInflightCircuitRequests() {
  if (amp_circuit_client_) {
    amp_circuit_client_->AbortInflight();
  }
  if (amp_circuit_hops_) {
    amp_circuit_hops_->ClearAll();
  }
}

} // namespace pp::mesh
