#include "domain/mesh/media_plane/MeshMediaPlane.h"

#include "domain/mesh/l4/media_relay/AmpMediaRelayClient.h"
#include "domain/mesh/reachability/AmpCircuitHopReach.h"
#include "domain/mesh/reachability/Reachability.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string PeerIdFromListenMultiaddr(const std::string& ma) {
  const auto p2p_pos = ma.rfind("/p2p/");
  if (p2p_pos == std::string::npos) {
    return {};
  }
  std::string peer_id = ma.substr(p2p_pos + 5);
  if (const auto slash = peer_id.find('/'); slash != std::string::npos) {
    peer_id.resize(slash);
  }
  return peer_id;
}

} // namespace

MeshMediaPlane::MeshMediaPlane() {
  redirectLogger("MeshMediaPlane");
}

MeshMediaPlane::~MeshMediaPlane() {
  Clear();
}

void MeshMediaPlane::SetDeps(MeshMediaPlaneDeps deps) {
  deps_ = std::move(deps);
  punch_.SetDeps({deps_.mesh, deps_.punch_introducers});
  CircuitRendezvousDeps rendezvous;
  rendezvous.mesh = deps_.mesh;
  rendezvous.rendezvous_candidates = deps_.rendezvous_candidates;
  rendezvous.bootstrap_seeds = deps_.bootstrap_seeds;
  rendezvous.last_good_relay = [this]() {
    ICircuitHopReach* reach = CircuitReach();
    return reach ? reach->LastGoodRelayPeerKey() : std::string();
  };
  rendezvous_.SetDeps(std::move(rendezvous));
}

void MeshMediaPlane::SetSignalingPunch(SignalingPunchFn punch) {
  punch_.SetSignalingPunch(std::move(punch));
}

void MeshMediaPlane::SetOnRelayChosen(std::function<void(const std::string&)> callback) {
  on_relay_chosen_ = std::move(callback);
}

// --- wiring ---------------------------------------------------------------------------------------

void MeshMediaPlane::Wire() {
  MeshHost* m = mesh();
  MeshIoContext io;
  if (auto chat = m ? m->ChatDeps() : std::nullopt) {
    // Exclusive Amp Drive: io_pump is empty; MeshPump (or a harness Tick loop) progresses Amp.
    io = chat->io;
  }
  WireMediaRelayClient(m, io);
  WireDialRegistry(m, io);
  WireCircuitHopReach(m, io);
  rendezvous_.InstallReparkListener();
}

bool MeshMediaPlane::AmpRelayAvailable() const {
  MeshHost* m = mesh();
  return m && m->Amp() && m->AmpMediaRelayCoord() && m->AmpMediaRelayCoord()->IsStarted();
}

void MeshMediaPlane::WireMediaRelayClient(MeshHost* m, const MeshIoContext& io) {
  if (!AmpRelayAvailable()) {
    media_relay_client_.reset();
    log().warning << "media-relay transport unavailable (Amp required)";
    return;
  }
  media_relay_client_ = std::make_unique<AmpMediaRelayClient>(*m->AmpMediaRelayCoord(), io.io_pump,
                                                              m->Amp()->LocalPeerId(), io.post_io, io.post_after);
  log().info << "media-relay transport=amp";
}

void MeshMediaPlane::WireDialRegistry(MeshHost* m, const MeshIoContext& io) {
  // Keep the dial registry stable across N025 listen sync — recreating mid-call drops answerer state.
  if (!dial_registry_) {
    dial_registry_ = std::make_unique<PeerSessionDialRegistry>();
  }
  const bool amp = AmpRelayAvailable();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  dial_registry_->SetAmpLinks(amp && chat ? &chat->links : nullptr);
  dial_registry_->SetAmpCircuitHops(amp && m->AmpCircuitHops() ? m->AmpCircuitHops() : nullptr);
  dial_registry_->SetPostIo(io.post_io);
}

void MeshMediaPlane::WireCircuitHopReach(MeshHost* m, const MeshIoContext& io) {
  const bool use_amp_circuit = AmpRelayAvailable() && m->AmpCircuitTunnel() && m->AmpCircuitTunnel()->IsStarted() &&
                               m->AmpCircuitHops();
  auto circuit = use_amp_circuit ? m->CircuitDeps() : std::nullopt;
  if (!circuit) {
    circuit_hop_reach_.reset();
    return;
  }
  auto reach = std::make_unique<AmpCircuitHopReach>(
      circuit->tunnel, circuit->hops, circuit->links, io.io_pump,
      // Reach's dial surface and punch step are the plane's reachability pieces (owned here, stable).
      [rendezvous = &rendezvous_](const std::string& exclude) { return rendezvous->DialableRelayIds(exclude); },
      [punch = &punch_](const std::string& target_peer_id, std::function<void(Roe<void>)> on_done) {
        punch->TryColdPunchAsync(target_peer_id, std::move(on_done));
      },
      [punch = &punch_](const std::string& introducer_peer_key, const std::string& target_peer_id,
                        std::function<void(Roe<void>)> on_done) {
        punch->TryUpgradePunchAsync(introducer_peer_key, target_peer_id, std::move(on_done));
      },
      io.post_io, io.post_after);
  reach->SetOnRelayChosen(deferred_.Bind([this](const std::string& relay_peer_key) {
    if (relay_peer_key.empty()) {
      return;
    }
    rendezvous_.NoteChosenRelay(relay_peer_key);
    log().info << "circuit rendezvous chosen R1=" << relay_peer_key;
    if (on_relay_chosen_) {
      on_relay_chosen_(relay_peer_key);
    }
  }));
  circuit_hop_reach_ = std::move(reach);
  log().info << "circuit-hop reach=amp";
}

void MeshMediaPlane::BindTestPath(IDialRegistry* dial, ICircuitHopReach* circuit_reach) {
  test_dial_ = dial;
  test_circuit_reach_ = circuit_reach;
}

IDialRegistry* MeshMediaPlane::Dial() const {
  return test_dial_ ? test_dial_ : dial_registry_.get();
}

ICircuitHopReach* MeshMediaPlane::CircuitReach() const {
  return test_circuit_reach_ ? test_circuit_reach_ : circuit_hop_reach_.get();
}

MediaRelayAttachPorts MeshMediaPlane::RelayAttachPorts() const {
  MediaRelayAttachPorts ports;
  ports.relay = media_relay_client_.get();
  ports.dial = Dial();
  ports.service_reach = CircuitReach();
  return ports;
}

void MeshMediaPlane::InvalidateAsyncOps() {
  rendezvous_.Invalidate();
  if (auto* amp = dynamic_cast<AmpCircuitHopReach*>(circuit_hop_reach_.get())) {
    amp->SetOnRelayChosen({});
  }
  deferred_.Invalidate();
}

void MeshMediaPlane::ResetRelayClient() {
  media_relay_client_.reset();
}

void MeshMediaPlane::ResetRelayClients() {
  media_relay_client_.reset();
  dial_registry_.reset();
}

void MeshMediaPlane::ResetAfterMeshStop() {
  media_relay_client_.reset();
  dial_registry_.reset();
  circuit_hop_reach_.reset();
}

void MeshMediaPlane::Clear() {
  InvalidateAsyncOps();
  media_relay_client_.reset();
  dial_registry_.reset();
  circuit_hop_reach_.reset();
  test_dial_ = nullptr;
  test_circuit_reach_ = nullptr;
  rendezvous_.Clear();
  peer_listen_mas_.clear();
}

// --- peer listen book -----------------------------------------------------------------------------

std::string MeshMediaPlane::RegisterPeerListenMultiaddrs(const std::string& key,
                                                         const std::vector<std::string>& multiaddrs) {
  if (key.empty() || multiaddrs.empty()) {
    return {};
  }
  const std::vector<std::string> ranked = RankAmpDialMultiaddrs(multiaddrs, CollectAmpDialLocalContext());
  std::vector<std::string>& stored = peer_listen_mas_[key];
  std::vector<std::string> dialable;
  for (const std::string& ma : ranked) {
    if (ma.empty()) {
      continue;
    }
    if (std::find(stored.begin(), stored.end(), ma) == stored.end()) {
      stored.push_back(ma);
    }
    if (IsLikelyUndialableLanIpv4(IpHostFromMultiaddrPrefix(ma))) {
      log().info << "Peer listen addr skipped undialable dial_key=" << key << " ma=" << ma;
      continue;
    }
    dialable.push_back(ma);
  }
  if (dialable.empty()) {
    return {};
  }
  const std::string peer_id = PeerIdFromListenMultiaddr(dialable.front());
  if (dial_registry_) {
    // B28: best-first through RegisterEndpoints (atomic DialBook replace). Single RegisterEndpoint
    // writes post async and out-of-order posts scramble Preferred / candidate order.
    (void)dial_registry_->RegisterEndpoints(key, dialable);
    dial_registry_->ClearDialBackoff(key);
    if (!peer_id.empty()) {
      dial_registry_->ClearDialBackoff(peer_id);
      if (deps_.note_lan_peer_id) {
        deps_.note_lan_peer_id(peer_id);
      }
    }
  } else if (deps_.register_direct_endpoint) {
    // No dial registry: worst → best single writes so the best ends Preferred.
    for (auto it = dialable.rbegin(); it != dialable.rend(); ++it) {
      deps_.register_direct_endpoint(key, *it);
      if (!peer_id.empty() && peer_id != key) {
        deps_.register_direct_endpoint(peer_id, *it);
      }
    }
  }
  for (const std::string& ma : dialable) {
    log().info << "Peer listen addr registered dial_key=" << key << " ma=" << ma;
  }
  return peer_id;
}

bool MeshMediaPlane::PeerLanConfirmed(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return false;
  }
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  return chat && chat->links.IsConnected(peer_id);
}

// --- reach ----------------------------------------------------------------------------------------

Roe<void> MeshMediaPlane::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (AppRuntime::IsShuttingDown()) {
    return Error("shutdown in progress");
  }
  if (!circuit_hop_reach_) {
    return Error("Amp circuit reach required");
  }
  return circuit_hop_reach_->TryEnsureHopReachable(hop_peer_id);
}

Roe<void> MeshMediaPlane::TryEnsurePeerReachable(const std::string& peer_key) {
  if (AppRuntime::IsShuttingDown()) {
    return Error("shutdown in progress");
  }
  if (!circuit_hop_reach_) {
    return Error("Amp circuit reach required");
  }
  if (peer_key.empty()) {
    return Error("missing peer");
  }
  return circuit_hop_reach_->TryEnsurePeerReachable(peer_key);
}

void MeshMediaPlane::TryEnsurePeerReachableAsync(const std::string& peer_key, std::function<void(Roe<void>)> on_done) {
  if (!on_done) {
    return;
  }
  if (AppRuntime::IsShuttingDown()) {
    on_done(Error("shutdown in progress"));
  } else if (!circuit_hop_reach_) {
    on_done(Error("Amp circuit reach required"));
  } else if (peer_key.empty()) {
    on_done(Error("missing peer"));
  } else {
    circuit_hop_reach_->TryEnsurePeerReachableAsync(peer_key, std::move(on_done));
  }
}

Roe<void> MeshMediaPlane::TryUpgradeToDirect(const std::string& peer_key) {
  if (!circuit_hop_reach_) {
    return Error("amp circuit reach required");
  }
  if (peer_key.empty()) {
    return Error("missing peer");
  }
  return circuit_hop_reach_->TryUpgradeToDirect(peer_key);
}

} // namespace pbr
