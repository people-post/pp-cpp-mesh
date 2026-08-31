#include "base/p2p/MeshHost.h"

#include "base/adp/Clock.h"
#include "base/adp/OsUdpDatagramIo.h"
#include "base/adp/Types.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/p2p/PeerIdUtil.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

amp::PeerLinkConfig MakeAmpLinkConfig() {
  amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const ByteVector& identity_public_key) -> std::string {
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

  runtime_ = std::make_unique<NodeRuntime>();
  if (auto started = runtime_->Start(config.runtime); !started) {
    last_error_ = runtime_->LastError().empty() ? started.error().message : runtime_->LastError();
    runtime_.reset();
    return started.error();
  }

  dial_back_ = std::make_unique<DialBackService>(*runtime_->Host(), *runtime_->Sessions());
  dial_back_->Start();

  circuit_relay_ = std::make_unique<CircuitRelayService>(*runtime_->Host(), *runtime_->Sessions());
  if (config.host_circuit_relay) {
    circuit_relay_->Start();
  }

  // Outbound client API always available; inbound hosting only when requested.
  media_relay_ = std::make_unique<MediaRelayService>(*runtime_->Host(), *runtime_->Sessions());
  media_relay_->SetBudget(config.media_relay_budget);
  media_relay_->SetPricing(config.media_relay_pricing);
  if (config.host_media_relay) {
    media_relay_->Start();
  }

  reachability_ = std::make_unique<ReachabilityService>();
  if (config.on_reachability_updated) {
    reachability_->SetOnUpdated(config.on_reachability_updated);
  }
  if (config.start_reachability_probe) {
    reachability_->StartProbe(*runtime_, *dial_back_, config.try_upnp_first);
  }

  if (config.enable_amp_stack) {
    amp_last_error_.clear();
    if (auto amp_started = StartAmpFromConfig(config); !amp_started) {
      // Soft-fail: keep libp2p up until L4 cutover (A023 parallel phase).
      amp_last_error_ = amp_started.error().message;
    }
  }
  return {};
}

Roe<void> MeshHost::StartAmpFromConfig(const MeshHostConfig& config) {
  const auto& host_cfg = config.runtime.host;
  if (!host_cfg.device_ml_dsa_private_key || !host_cfg.device_ml_dsa_public_key) {
    return Error("mesh host: enable_amp_stack requires device ML-DSA keys (shared PeerId with libp2p)");
  }

  amp::MshIdentity identity;
  identity.ml_dsa_secret_key.assign(host_cfg.device_ml_dsa_private_key->begin(),
                                    host_cfg.device_ml_dsa_private_key->end());
  identity.ml_dsa_public_key.assign(host_cfg.device_ml_dsa_public_key->begin(),
                                    host_cfg.device_ml_dsa_public_key->end());

  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  auto bound = adp::OsUdpDatagramIo::Bind(adp::IpEndpoint::V4(0, 0, 0, 0, config.amp_udp_port));
  if (!bound) {
    return bound.error();
  }
  std::shared_ptr<adp::DatagramIo> io = std::move(*bound);
  amp_clock_ = std::make_shared<adp::WallClock>();

  amp::AmpStack::Config amp_cfg;
  amp_cfg.identity = std::move(identity);
  amp_cfg.local_peer_id = *peer_id;
  amp_cfg.link_config = MakeAmpLinkConfig();

  auto stack = amp::AmpStack::Create(std::move(io), amp_clock_, std::move(amp_cfg));
  if (!stack) {
    return stack.error();
  }

  auto listen = amp::FormatAdpMultiaddr((*stack)->LocalEndpoint(), *peer_id);
  if (!listen) {
    return listen.error();
  }

  (*stack)->Start();
  (*stack)->GetEndpoint().SetAcceptEnabled(config.runtime.host.listen_enabled);
  amp_listen_multiaddr_ = *listen;
  (*stack)->Links().SetLocalListenMultiaddrs({amp_listen_multiaddr_});
  amp_ = std::move(*stack);
  ApplyAmpAdvertisement(config);
  EnsureAmpL4Coordinators();
  StartAmpL4Hosting(config.host_circuit_relay, config.host_media_relay);
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
}

void MeshHost::StartAmpL4Hosting(const bool host_circuit, const bool host_media) {
  EnsureAmpL4Coordinators();
  // Always Start so SoftMigrate guests / circuit clients can dial; inbound hosting is gated.
  if (amp_circuit_ && !amp_circuit_->IsStarted()) {
    amp_circuit_->Start();
  }
  if (amp_media_relay_ && !amp_media_relay_->IsStarted()) {
    amp_media_relay_->Start();
  }
  if (amp_circuit_) {
    amp_circuit_->SetServeInbound(host_circuit);
  }
  if (amp_media_relay_) {
    amp_media_relay_->SetServeInbound(host_media);
  }
}

void MeshHost::StopAmp() {
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
  amp_clock_.reset();
  amp_listen_multiaddr_.clear();
  amp_last_error_.clear();
}

Roe<void> MeshHost::AttachAmpStack(std::unique_ptr<amp::AmpStack> stack, std::string listen_multiaddr) {
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
  EnsureAmpL4Coordinators();
  // Tests / AttachAmpStack: start outbound-capable L4 without inbound hosting unless configured.
  StartAmpL4Hosting(false, false);
  return Roe<void>();
}

void MeshHost::ApplyAmpAdvertisement(const MeshHostConfig& config) {
  if (!amp_) {
    return;
  }
  std::vector<std::string> protocols = {"/pp-browser/chat/1.0.0", "/pp-browser/chat-history/1.0.0",
                                        "/pp-browser/call-media/1.0.0", kDialBackProtocolId};
  if (config.host_circuit_relay) {
    protocols.push_back("/pp-browser/circuit-relay/1.0.0");
  }
  if (config.host_media_relay) {
    protocols.push_back("/pp-browser/media-relay/1.0.0");
  }
  amp_->Links().SetAdvertisedProtocols(std::move(protocols));
}

void MeshHost::Stop() {
  if (circuit_relay_) {
    circuit_relay_->AbortInflightRequests();
  }
  if (media_relay_) {
    media_relay_->Stop();
    media_relay_.reset();
  }
  if (circuit_relay_) {
    circuit_relay_->Stop();
    circuit_relay_.reset();
  }
  if (dial_back_) {
    dial_back_->Stop();
    dial_back_.reset();
  }
  if (runtime_) {
    runtime_->Stop();
    runtime_.reset();
  }
  StopAmp();
  // Keep a fresh, valid ReachabilityService so Reachability() references stay safe.
  reachability_ = std::make_unique<ReachabilityService>();
}

void MeshHost::Tick() {
  if (runtime_) {
    runtime_->Tick();
  }
  if (amp_) {
    amp_->Pump();
    amp_->Tick();
  }
}

bool MeshHost::IsRunning() const { return runtime_ && runtime_->IsRunning(); }

NodeRuntime* MeshHost::Runtime() { return runtime_.get(); }

Libp2pHost* MeshHost::Host() { return runtime_ ? runtime_->Host() : nullptr; }

PeerSessionManager* MeshHost::Sessions() const { return runtime_ ? runtime_->Sessions() : nullptr; }

DialBackService* MeshHost::DialBack() { return dial_back_.get(); }

CircuitRelayService* MeshHost::CircuitRelay() { return circuit_relay_.get(); }

MediaRelayService* MeshHost::MediaRelay() { return media_relay_.get(); }

ReachabilityService& MeshHost::Reachability() { return *reachability_; }

amp::AmpStack* MeshHost::Amp() { return amp_.get(); }

const amp::AmpStack* MeshHost::Amp() const { return amp_.get(); }

CircuitTunnelCoordinator* MeshHost::AmpCircuitTunnel() { return amp_circuit_.get(); }

AmpMediaRelayCoordinator* MeshHost::AmpMediaRelayCoord() { return amp_media_relay_.get(); }

AmpCircuitHopRegistry* MeshHost::AmpCircuitHops() { return amp_circuit_hops_.get(); }

const std::string& MeshHost::BoundListenMultiaddr() const {
  static const std::string kEmpty;
  return runtime_ ? runtime_->BoundListenMultiaddr() : kEmpty;
}

void MeshHost::AbortInflightCircuitRequests() {
  if (circuit_relay_) {
    circuit_relay_->AbortInflightRequests();
  }
  if (amp_circuit_) {
    amp_circuit_->AbortInflight();
  }
  if (amp_circuit_hops_) {
    amp_circuit_hops_->ClearAll();
  }
}

} // namespace pbr
