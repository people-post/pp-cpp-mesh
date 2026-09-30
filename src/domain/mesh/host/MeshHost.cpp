#include "domain/mesh/reachability/ReachabilityNetIf.h"
#include "domain/mesh/reachability/AmpObservedAddrs.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/mesh/host/AmpLinkConfig.h"
#include "domain/mesh/host/MeshLinkEventLog.h"
#include "domain/mesh/reachability/dial_back/DialBackTypes.h"
#include "domain/mesh/reachability/punch/PunchTypes.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/thread/ChatBlobTypes.h"

#include "domain/mesh/host/MeshPorts.h"

#include <utility>

#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L2/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "foundation/identity/PeerIdUtil.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

logging::Logger& MeshHostLog() {
  static logging::Logger logger = logging::getLogger("MeshHost");
  return logger;
}

} // namespace

MeshHost::MeshHost() : reachability_(std::make_unique<ReachabilityEngine>()) {}

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
  // Runs on the Connectivity owner: the link / punch updates belong to the Amp IO strand.
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

  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  // Always dual-stack :: (call-path-resilience K010): the socket then serves IPv4 and IPv6 for the
  // whole run, so moving from an IPv4-only network to IPv6-only cellular (NAT64) needs no rebind.
  // pp-cpp-amp clears IPV6_V6ONLY and maps IPv4 peers both ways; advertise / probe targets come
  // from the interfaces, not the bind family. IPv4 only when the OS has no IPv6 socket support.
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V6({}, config.amp_udp_port));
  if (!bound) {
    bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(0, 0, 0, 0, config.amp_udp_port));
  }
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  amp_clock_ = std::make_shared<pp::adp::WallClock>();

  pp::amp::AmpStack::Config amp_cfg;
  amp_cfg.identity = std::move(identity);
  amp_cfg.local_peer_id = *peer_id;
  amp_cfg.link_config = MakeProductAmpLinkConfig();

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
  amp_listen_multiaddr_ = *listen;
  (*stack)->Links().SetLocalListenMultiaddrs({amp_listen_multiaddr_});
  // Every protocol this host serves registers a handler: refuse opens for anything else, so a peer
  // asking for a service we do not run (e.g. broadcast admission on a plain relay / pp-node) fails
  // at once instead of waiting out its request timeout.
  (*stack)->Runtime().SetRefuseUnhandledOpens(true);
  amp_ = std::move(*stack);
  InstallMeshLinkEventLog(amp_->Runtime());
  InstallMeshLinkMetrics(amp_->Runtime());
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
    pump_.Start([this]() { Tick(); });
  }
}

void MeshHost::StopOwnedThreads() {
  pump_.Stop();
  l4_on_workers_.store(false, std::memory_order_release);
}

std::function<void(std::function<void()>)> MeshHost::MakeL4WorkerPost(const WorkerLane lane) const {
  return [this, lane](std::function<void()> task) {
    if (!task) {
      return;
    }
    if (l4_on_workers_.load(std::memory_order_acquire)) {
      AppRuntime::PostWorker(lane, std::move(task));
    } else {
      task();
    }
  };
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
    amp_circuit_server_ = std::make_unique<CircuitRelayServer>(amp_->Runtime());
  }
  if (!amp_circuit_client_) {
    amp_circuit_client_ = std::make_unique<CircuitClientCoordinator>(amp_->Runtime());
  }
  if (!amp_media_relay_server_) {
    amp_media_relay_server_ = std::make_unique<MediaRelayServer>(amp_->Runtime());
  }
  if (!amp_media_relay_client_) {
    amp_media_relay_client_ =
        std::make_unique<MediaRelayClientCoordinator>(amp_->Runtime(), amp_media_relay_server_.get());
  }
  amp_media_relay_client_->SetCircuitHopRegistry(amp_circuit_hops_.get());
  auto io_pump = MakeL4IoPump();
  auto post_worker = MakeL4WorkerPost(WorkerLane::Normal);
  if (!amp_dial_back_server_) {
    amp_dial_back_server_ = std::make_unique<DialBackServer>(amp_->Runtime());
  }
  if (!amp_dial_back_) {
    amp_dial_back_ = std::make_unique<DialBackClient>(amp_->Runtime(), io_pump);
  }
  if (!amp_punch_) {
    amp_punch_ = std::make_unique<AmpPunchCoordinator>(amp_->Runtime(), io_pump);
    amp_punch_->SetAddressDisclosure(address_disclosure_);
  }
  if (!amp_dht_) {
    amp_dht_ = std::make_unique<AmpDhtProtocol>(amp_->Runtime(), post_worker);
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
    amp_dht_->Start();
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
  amp_dht_.reset();
  amp_media_relay_client_.reset();
  amp_media_relay_server_.reset();
  amp_circuit_client_.reset();
  amp_circuit_server_.reset();
  amp_circuit_hops_.reset();
  if (amp_) {
    amp_->Stop();
    amp_.reset();
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
  InstallMeshLinkMetrics(amp_->Runtime());
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
  std::vector<std::string> protocols = {kDirectChatProtocolId, kChatHistoryProtocolId, kChatBlobProtocolId,
                                        kRpcPeerAnnounceProtocolId,
                                        kRpcBroadcastProtocolId,
                                        kCallMediaDirectProtocolId, kDialBackProtocolId, kAmpPunchProtocolId};
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
  // Retire the probe first (its destructor waits on the Connectivity owner), so no `on_updated`
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
    // exclusive Amp Drive (THREADING.md). Nested Drive is refused by MeshRuntime.
    amp_->Runtime().Drive();
  }
  if (amp_dht_) {
    amp_dht_->Tick();
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
}

AmpDhtProtocol* MeshHost::AmpDht() { return amp_dht_.get(); }

AmpDirectoryProtocol* MeshHost::AmpDirectory() { return amp_directory_.get(); }

void MeshHost::ConfigureAmpDht(AmpDhtProtocolConfig config) {
  if (!amp_dht_) {
    return;
  }
  amp_dht_->Configure(std::move(config));
}

void MeshHost::RefreshAmpDhtHosting(const bool host_dht) {
  host_dht_ = host_dht;
  if (!amp_) {
    return;
  }
  MeshHostConfig ad_cfg;
  ad_cfg.host_circuit_relay = amp_circuit_server_ && amp_circuit_server_->ServeInbound();
  ad_cfg.host_media_relay = amp_media_relay_server_ && amp_media_relay_server_->ServeInbound();
  ad_cfg.host_dht = host_dht_;
  ad_cfg.host_directory = host_directory_;
  ApplyAmpAdvertisement(ad_cfg);
  if (amp_dht_ && host_dht) {
    amp_dht_->Tick();
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
  MeshHostConfig ad_cfg;
  ad_cfg.host_circuit_relay = amp_circuit_server_ && amp_circuit_server_->ServeInbound();
  ad_cfg.host_media_relay = amp_media_relay_server_ && amp_media_relay_server_->ServeInbound();
  ad_cfg.host_dht = host_dht_;
  ad_cfg.host_directory = host_directory_;
  ApplyAmpAdvertisement(ad_cfg);
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
  deps.post_worker = MakeL4WorkerPost(WorkerLane::Background);  // UPnP discovery blocks (~2 s)
  deps.post_io = MakeL4IoPost();
  deps.post_after = MakeL4IoAfter();
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
  io.post_worker = MakeL4WorkerPost(WorkerLane::Normal);
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

} // namespace pbr
