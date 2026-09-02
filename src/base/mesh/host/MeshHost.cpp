#include "base/mesh/host/MeshHost.h"

#include "base/mesh/host/MeshPorts.h"

#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L2/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "base/mesh/identity/PeerIdUtil.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

pp::amp::PeerLinkConfig MakeAmpLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pp::amp::ByteVector& identity_public_key) -> std::string {
    auto peer_id = PeerIdFromMlDsaPublicKey(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  return config;
}

} // namespace

MeshHost::MeshHost() : reachability_(std::make_unique<ReachabilityService>()) {}

MeshHost::~MeshHost() { Stop(); }

Roe<void> MeshHost::Start(const MeshHostConfig& config) {
  Stop();
  last_error_.clear();
  amp_last_error_.clear();

  // D10/A017: Amp is the only product underlay. mesh_enabled=false leaves mesh off.
  if (!config.mesh_enabled) {
    reachability_ = std::make_unique<ReachabilityService>();
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
  reachability_ = std::make_unique<ReachabilityService>();
  if (config.on_reachability_updated) {
    reachability_->SetOnUpdated(config.on_reachability_updated);
  }
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

  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(0, 0, 0, 0, config.amp_udp_port));
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  amp_clock_ = std::make_shared<pp::adp::WallClock>();

  pp::amp::AmpStack::Config amp_cfg;
  amp_cfg.identity = std::move(identity);
  amp_cfg.local_peer_id = *peer_id;
  amp_cfg.link_config = MakeAmpLinkConfig();

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
  amp_ = std::move(*stack);
  chat_links_ = NewAmpChatPeerLinks(amp_->Links());
  ApplyAmpAdvertisement(config);
  EnsureAmpL4Coordinators();
  host_dht_ = config.host_dht;
  StartAmpL4Hosting(config.host_circuit_relay, config.host_media_relay, config.host_dht);
  return Roe<void>();
}

void MeshHost::EnsureAmpL4Coordinators() {
  if (!amp_) {
    return;
  }
  if (!amp_circuit_hops_) {
    amp_circuit_hops_ = std::make_unique<AmpCircuitHopRegistry>();
  }
  if (!amp_circuit_) {
    amp_circuit_ = std::make_unique<CircuitTunnelCoordinator>(amp_->Runtime());
  }
  if (!amp_media_relay_) {
    amp_media_relay_ = std::make_unique<AmpMediaRelayCoordinator>(amp_->Runtime());
  }
  amp_media_relay_->SetCircuitHopRegistry(amp_circuit_hops_.get());
  if (!amp_dial_back_) {
    AmpDialBackService::IoPump pump = [this]() { Tick(); };
    amp_dial_back_ = std::make_unique<AmpDialBackService>(amp_->Links(), std::move(pump));
  }
  if (!amp_dht_) {
    AmpDhtService::IoPump pump = [this]() { Tick(); };
    amp_dht_ = std::make_unique<AmpDhtService>(amp_->Links(), std::move(pump));
  }
}

void MeshHost::StartAmpL4Hosting(const bool host_circuit, const bool host_media, const bool host_dht) {
  EnsureAmpL4Coordinators();
  host_dht_ = host_dht;
  // Always accept nested Session carriers so NAT call-media (A024) works without hosting circuit.
  if (amp_) {
    amp_->Links().EnableNestedCarrierAccept(true);
  }
  // Always Start so SoftMigrate guests / circuit clients can dial; inbound hosting is gated.
  if (amp_circuit_ && !amp_circuit_->IsStarted()) {
    amp_circuit_->Start();
  }
  if (amp_media_relay_ && !amp_media_relay_->IsStarted()) {
    amp_media_relay_->Start();
  }
  if (amp_dial_back_ && !amp_dial_back_->IsStarted()) {
    amp_dial_back_->Start();
  }
  if (amp_dht_ && !amp_dht_->IsStarted()) {
    amp_dht_->Start();
  }
  if (amp_circuit_) {
    amp_circuit_->SetServeInbound(host_circuit);
  }
  if (amp_media_relay_) {
    amp_media_relay_->SetServeInbound(host_media);
  }
  ApplyAmpAdvertisement(MeshHostConfig{.host_circuit_relay = host_circuit, .host_media_relay = host_media,
                                       .host_dht = host_dht});
}

void MeshHost::StopAmp() {
  if (amp_) {
    amp_->Links().EnableNestedCarrierAccept(false);
  }
  if (amp_dial_back_) {
    amp_dial_back_->Stop();
    amp_dial_back_.reset();
  }
  if (amp_dht_) {
    amp_dht_->Stop();
    amp_dht_.reset();
  }
  if (amp_media_relay_) {
    amp_media_relay_->Stop();
    amp_media_relay_.reset();
  }
  if (amp_circuit_) {
    amp_circuit_->Stop();
    amp_circuit_.reset();
  }
  if (amp_circuit_hops_) {
    amp_circuit_hops_->ClearAll();
    amp_circuit_hops_.reset();
  }
  if (amp_) {
    amp_->Stop();
    amp_.reset();
  }
  chat_links_.reset();
  amp_clock_.reset();
  amp_listen_multiaddr_.clear();
  amp_last_error_.clear();
}

Roe<void> MeshHost::AttachAmpStack(std::unique_ptr<pp::amp::AmpStack> stack, std::string listen_multiaddr) {
  if (!stack) {
    return Error("mesh host: null AmpStack");
  }
  StopAmp();
  amp_ = std::move(stack);
  amp_->Start();
  amp_listen_multiaddr_ = std::move(listen_multiaddr);
  if (!amp_listen_multiaddr_.empty()) {
    amp_->Links().SetLocalListenMultiaddrs({amp_listen_multiaddr_});
  }
  chat_links_ = NewAmpChatPeerLinks(amp_->Links());
  EnsureAmpL4Coordinators();
  // Tests / AttachAmpStack: start outbound-capable L4 without inbound hosting unless configured.
  StartAmpL4Hosting(false, false, false);
  return Roe<void>();
}

void MeshHost::ApplyAmpAdvertisement(const MeshHostConfig& config) {
  if (!amp_) {
    return;
  }
  std::vector<std::string> protocols = {"/pp-browser/chat/1.0.0", "/pp-browser/chat-history/1.0.0",
                                        "/pp-browser/chat-blob/1.0.0", "/pp-browser/call-media/1.0.0",
                                        "/pp-browser/dial-back/1.0.0"};
  if (config.host_circuit_relay) {
    protocols.push_back("/pp-browser/circuit-relay/1.0.0");
  }
  if (config.host_media_relay) {
    protocols.push_back("/pp-browser/media-relay/1.0.0");
  }
  if (config.host_dht) {
    protocols.push_back(kDhtProtocolId);
  }
  amp_->Links().SetAdvertisedProtocols(std::move(protocols));
}

void MeshHost::Stop() {
  if (amp_circuit_) {
    amp_circuit_->AbortInflight();
  }
  if (amp_media_relay_) {
    amp_media_relay_->AbortInflight();
  }
  StopAmp();
  bootstrap_peers_.clear();
  // Keep a fresh, valid ReachabilityService so Reachability() references stay safe.
  reachability_ = std::make_unique<ReachabilityService>();
}

void MeshHost::Tick() {
  if (amp_) {
    // Single locked Drive: Connect waiters (worker) and TickMesh (coordinator) both call Tick.
    amp_->Runtime().Drive();
  }
}

bool MeshHost::IsRunning() const { return static_cast<bool>(amp_); }

AmpDialBackService* MeshHost::AmpDialBack() { return amp_dial_back_.get(); }

AmpDhtService* MeshHost::AmpDht() { return amp_dht_.get(); }

void MeshHost::ConfigureAmpDht(AmpDhtServiceConfig config) {
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
  ad_cfg.host_circuit_relay = amp_circuit_ && amp_circuit_->ServeInbound();
  ad_cfg.host_media_relay = amp_media_relay_ && amp_media_relay_->ServeInbound();
  ad_cfg.host_dht = host_dht_;
  ApplyAmpAdvertisement(ad_cfg);
  if (amp_dht_ && host_dht) {
    amp_dht_->Tick();
  }
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
  deps.io_pump = [self = const_cast<MeshHost*>(this)]() { self->Tick(); };
  deps.try_upnp_first = try_upnp_first;
  return deps;
}

void MeshHost::StartReachabilityProbe(bool try_upnp_first) {
  if (!amp_ || !amp_dial_back_) {
    return;
  }
  reachability_->StartProbe(MakeReachabilityDeps(try_upnp_first));
}

void MeshHost::RunReachabilityProbeBlocking(bool try_upnp_first) {
  if (!amp_ || !amp_dial_back_) {
    return;
  }
  reachability_->RunProbeBlocking(MakeReachabilityDeps(try_upnp_first));
}

ReachabilityService& MeshHost::Reachability() { return *reachability_; }

std::optional<MeshChatDeps> MeshHost::ChatDeps() {
  if (!amp_ || !chat_links_) {
    return std::nullopt;
  }
  MeshIoContext io;
  io.io_pump = [this]() { Tick(); };
  io.local_peer_id = amp_->LocalPeerId();
  io.listen_multiaddr = amp_listen_multiaddr_;
  return MeshChatDeps{std::move(io), *chat_links_};
}

std::optional<MeshCircuitDeps> MeshHost::CircuitDeps() {
  if (!amp_ || !chat_links_ || !amp_circuit_ || !amp_circuit_hops_) {
    return std::nullopt;
  }
  return MeshCircuitDeps{*amp_circuit_, *amp_circuit_hops_, *chat_links_};
}

pp::amp::AmpStack* MeshHost::Amp() { return amp_.get(); }

const pp::amp::AmpStack* MeshHost::Amp() const { return amp_.get(); }

CircuitTunnelCoordinator* MeshHost::AmpCircuitTunnel() { return amp_circuit_.get(); }

AmpMediaRelayCoordinator* MeshHost::AmpMediaRelayCoord() { return amp_media_relay_.get(); }

AmpCircuitHopRegistry* MeshHost::AmpCircuitHops() { return amp_circuit_hops_.get(); }

void MeshHost::AbortInflightCircuitRequests() {
  if (amp_circuit_) {
    amp_circuit_->AbortInflight();
  }
  if (amp_circuit_hops_) {
    amp_circuit_hops_->ClearAll();
  }
}

} // namespace pbr
