#include "base/p2p/MeshHost.h"

namespace pbr {

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
  return {};
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
  // Keep a fresh, valid ReachabilityService so Reachability() references stay safe.
  reachability_ = std::make_unique<ReachabilityService>();
}

void MeshHost::Tick() {
  if (runtime_) {
    runtime_->Tick();
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

const std::string& MeshHost::BoundListenMultiaddr() const {
  static const std::string kEmpty;
  return runtime_ ? runtime_->BoundListenMultiaddr() : kEmpty;
}

void MeshHost::AbortInflightCircuitRequests() {
  if (circuit_relay_) {
    circuit_relay_->AbortInflightRequests();
  }
}

} // namespace pbr
