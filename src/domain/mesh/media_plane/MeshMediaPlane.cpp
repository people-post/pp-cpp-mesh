#include "domain/mesh/media_plane/MeshMediaPlane.h"

#include "common/directory/MeshHopDial.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitRendezvousPolicy.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/l4/media_relay/AmpMediaRelayClient.h"
#include "domain/mesh/reachability/AmpCircuitHopReach.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "domain/mesh/reachability/Reachability.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/**
 * A rendezvous relay must be directly reachable: a peer we only reach through a relay carrier
 * (typically the call peer itself) cannot relay for us — reserving on it just sends op=reserve
 * over the peer's own carrier ("circuit-relay service not ready"), every renewal (dogfood 16:17).
 */
bool ReachableOnlyViaCarrier(IChatPeerLinks& links, const std::string& peer_id) {
  const pp::amp::LinkSnapshotEx snap = links.SnapshotByPeerId(peer_id);
  return snap.transport == pp::amp::TransportClass::Carrier && snap.base.phase == pp::amp::PeerLinkPhase::Connected;
}

void CompletePunch(const std::function<void(Roe<void>)>& on_done, AmpPunchCoordinator::PunchRoe punched,
                   const char* fail_fallback) {
  if (!punched) {
    on_done(Error(punched.error().message));
  } else if (!punched->ok) {
    on_done(Error(punched->error.empty() ? fail_fallback : punched->error));
  } else {
    on_done(Roe<void>());
  }
}

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

void RegisterDialableEndpoint(IChatPeerLinks& links, const MeshHopCandidate& hop) {
  if (!hop.peer_id.empty() && !hop.multiaddr.empty() && IsAdpMultiaddr(hop.multiaddr) &&
      CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
    (void)links.RegisterEndpoint(hop.peer_id, hop.multiaddr);
  }
}

} // namespace

/** One EnsureBootstrapSeedParkedAsync wait: settles once (listener, deadline or immediate check). */
struct MeshMediaPlane::SeedPark {
  std::atomic<bool> settled{false};
  pp::amp::PeerLinkManager* links = nullptr;
  std::atomic<pp::amp::PeerLinkManager::PeerConnectedListenerId> listener_id{0};
  std::atomic<uint64_t> deadline_timer{0};
  std::function<void(bool)> on_done;
};

/** Serial cold dial + reserve over the rest of the rendezvous surface (bounded by cold_limit). */
struct MeshMediaPlane::ColdPark {
  std::vector<MeshHopCandidate> hops;
  size_t cold_limit = 0;
};

MeshMediaPlane::MeshMediaPlane() {
  redirectLogger("MeshMediaPlane");
}

MeshMediaPlane::~MeshMediaPlane() {
  Clear();
}

void MeshMediaPlane::SetDeps(MeshMediaPlaneDeps deps) {
  deps_ = std::move(deps);
}

void MeshMediaPlane::SetSignalingPunch(SignalingPunchFn punch) {
  signaling_punch_ = std::move(punch);
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
  InstallRendezvousReparkListener();
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
      [this](const std::string& exclude) { return CollectDialableCircuitRelayIds(exclude); },
      [this](const std::string& target_peer_id, std::function<void(Roe<void>)> on_done) {
        TryColdPunchAsync(target_peer_id, std::move(on_done));
      },
      [this](const std::string& introducer_peer_key, const std::string& target_peer_id,
             std::function<void(Roe<void>)> on_done) {
        TryUpgradePunchAsync(introducer_peer_key, target_peer_id, std::move(on_done));
      },
      io.post_io, io.post_after);
  reach->SetOnRelayChosen(deferred_.Bind([this](const std::string& relay_peer_key) {
    if (relay_peer_key.empty()) {
      return;
    }
    chosen_circuit_r1_ = relay_peer_key;
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
  RemoveRendezvousReparkListener();
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
  chosen_circuit_r1_.clear();
  peer_listen_mas_.clear();
}

void MeshMediaPlane::PostIoOrRun(std::function<void()> task) const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (chat && chat->io.post_io) {
    chat->io.post_io(std::move(task));
  } else {
    task();
  }
}

// --- punch ----------------------------------------------------------------------------------------

void MeshMediaPlane::TryColdPunchAsync(const std::string& target_peer_id, std::function<void(Roe<void>)> on_done) {
  MeshHost* m = mesh();
  auto circuit = m ? m->CircuitDeps() : std::nullopt;
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!circuit || !punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  IChatPeerLinks* links = &circuit->links;
  MeshPunchIntroducers introducers = deps_.punch_introducers ? deps_.punch_introducers() : MeshPunchIntroducers{};
  auto has_ep = [links](const std::string& id) { return links->GetLinkSnapshot(id).has_endpoint; };
  auto is_conn = [links](const std::string& id) { return links->IsConnected(id); };

  // B29: if the first introducer misses (target unknown / channel fail), try the next. The attempt
  // holds its own retry function; the chain ends when it settles (no self-owning cycle past that).
  struct IntroAttempt {
    std::unordered_set<std::string> tried;
    std::function<void()> try_next;
  };
  auto attempt = std::make_shared<IntroAttempt>();
  std::weak_ptr<IntroAttempt> weak = attempt;
  attempt->try_next = [this, weak, punch, target_peer_id, introducers = std::move(introducers), has_ep, is_conn,
                       on_done = std::move(on_done)]() {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    auto intro = PickPunchIntroducer(introducers.contact_peer_ids, introducers.seed_peer_ids, target_peer_id, has_ep,
                                     is_conn, self->tried);
    if (!intro) {
      if (signaling_punch_) {
        log().info << "punch introducers exhausted — H012 signaling fallback target=" << target_peer_id;
        signaling_punch_(target_peer_id, punch->LocalCandidateAddrs(), on_done);
        return;
      }
      on_done(Error(self->tried.empty() ? "no punch introducer" : "punch introducers exhausted"));
      return;
    }
    self->tried.insert(*intro);
    punch->TryColdPunchAsync(
        *intro, target_peer_id, punch->LocalCandidateAddrs(),
        [self, on_done](AmpPunchCoordinator::PunchRoe punched) {
          if (punched && punched->ok) {
            CompletePunch(on_done, std::move(punched), "punch failed");
            return;
          }
          const std::string err =
              !punched ? punched.error().message : (punched->error.empty() ? std::string("punch failed") : punched->error);
          const bool try_another = err.find("introducer") != std::string::npos ||
                                   err.find("not registered") != std::string::npos;
          if (try_another) {
            self->try_next();
            return;
          }
          on_done(Error(err));
        },
        2000);
  };
  attempt->try_next();
}

void MeshMediaPlane::TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                                          std::function<void(Roe<void>)> on_done) {
  MeshHost* m = mesh();
  auto* punch = m ? m->AmpPunch() : nullptr;
  if (!punch || !punch->IsStarted()) {
    on_done(Error("amp punch unavailable"));
    return;
  }
  punch->TryUpgradePunchAsync(
      introducer_peer_key, target_peer_id, punch->LocalCandidateAddrs(),
      [on_done = std::move(on_done)](AmpPunchCoordinator::PunchRoe punched) {
        CompletePunch(on_done, std::move(punched), "upgrade punch failed");
      },
      2000);
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

// --- rendezvous candidates ------------------------------------------------------------------------

std::vector<MeshHopCandidate> MeshMediaPlane::RendezvousCandidates(const std::string& exclude_peer_id) const {
  std::vector<MeshHopCandidate> hops =
      deps_.rendezvous_candidates ? deps_.rendezvous_candidates() : std::vector<MeshHopCandidate>{};
  // B41: the merged surface can contain this node's own PeerId (and the target's); reserving on
  // ourselves burns a 7-15 s dial budget per attempt.
  std::string self_peer_id;
  if (MeshHost* m = mesh(); m && m->Amp()) {
    self_peer_id = m->Amp()->LocalPeerId();
  }
  std::erase_if(hops, [&](const MeshHopCandidate& hop) {
    return hop.peer_id.empty() || hop.peer_id == exclude_peer_id || (!self_peer_id.empty() && hop.peer_id == self_peer_id);
  });
  return hops;
}

std::vector<std::string> MeshMediaPlane::CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const {
  std::vector<std::string> relay_ids;
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  IChatPeerLinks* links = chat ? &chat->links : nullptr;
  AmpCircuitHopRegistry* amp_hops = m ? m->AmpCircuitHops() : nullptr;
  if (!links && !amp_hops) {
    return relay_ids;
  }
  for (const MeshHopCandidate& hop : RendezvousCandidates(exclude_peer_id)) {
    // Dogfood: directory/contact hop MAs are often RFC1918 advertise addrs. Unconditional
    // RegisterEndpoint overwrites a seed-warmed public PreferredMultiaddr and StartBridge
    // then fails with `adp udp :send to`. Only write dialable hosts; keep existing endpoint.
    if (links && !hop.multiaddr.empty()) {
      RegisterDialableEndpoint(*links, hop);
    } else if (links) {
      if (auto ma = links->PreferredMultiaddr(hop.peer_id); ma && CircuitHopDialBookAllowsRegister(*ma)) {
        (void)links->RegisterEndpoint(hop.peer_id, *ma);
      }
    }
    bool amp_ok = false;
    if (links && links->GetLinkSnapshot(hop.peer_id).has_endpoint) {
      // Capability ingest can replace a public seed Preferred with /ip4/0.0.0.0 listen
      // (dogfood 084055). Skip undialable Preferred unless already Connected (peer-id-only).
      if (links->IsConnected(hop.peer_id)) {
        amp_ok = true;
      } else if (auto ma = links->PreferredMultiaddr(hop.peer_id)) {
        amp_ok = CircuitHopMultiaddrIsUdpDialable(*ma);
      }
    }
    if (amp_ok || (amp_hops && amp_hops->HasAny(hop.peer_id))) {
      relay_ids.push_back(hop.peer_id);
    }
  }
  return relay_ids;
}

std::vector<std::string> MeshMediaPlane::BootstrapSeedPeerIds() const {
  std::vector<std::string> out;
  for (const auto& hop : deps_.bootstrap_seeds ? deps_.bootstrap_seeds() : std::vector<MeshHopCandidate>{}) {
    if (!hop.peer_id.empty()) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

bool MeshMediaPlane::AnyBootstrapSeedConnectedOnIo() const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat) {
    return false;
  }
  const auto ids = BootstrapSeedPeerIds();
  return std::any_of(ids.begin(), ids.end(), [&](const std::string& id) { return chat->links.IsConnected(id); });
}

bool MeshMediaPlane::AllBootstrapSeedsConnectedOnIo() const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  const auto ids = BootstrapSeedPeerIds();
  if (!chat || ids.empty()) {
    return false;
  }
  return std::all_of(ids.begin(), ids.end(), [&](const std::string& id) { return chat->links.IsConnected(id); });
}

// --- warm / reserve -------------------------------------------------------------------------------

void MeshMediaPlane::WarmBootstrapSeedSessions() {
  MeshHost* m = mesh();
  if (!m || !m->ChatDeps()) {
    return;
  }
  // EnsureAssociation / RegisterEndpoint run on Amp IO (dogfood 085210 Coordinator vs MeshPump).
  PostIoOrRun(deferred_.Bind([this]() { WarmBootstrapSeedSessionsOnIo(); }));
}

void MeshMediaPlane::WarmBootstrapSeedSessionsOnIo() {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat) {
    return;
  }
  const auto hops = deps_.bootstrap_seeds ? deps_.bootstrap_seeds() : std::vector<MeshHopCandidate>{};
  // Register public bootstrap MAs; dial at most one cold seed (serial). Parallel EnsureAssociation
  // on both Brief hops + peer Preferred contended on ADP UDP (dogfood fd4e3de).
  for (const auto& hop : hops) {
    RegisterDialableEndpoint(chat->links, hop);
  }
  for (const auto& hop : hops) {
    if (hop.peer_id.empty()) {
      continue;
    }
    if (!chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint) {
      log().info << "bootstrap warm skip peer=" << hop.peer_id << " reason=!endpoint";
      continue;
    }
    if (chat->links.IsConnected(hop.peer_id)) {
      log().info << "bootstrap warm already connected peer=" << hop.peer_id;
      continue;  // still warm remaining seeds (dialer may pick hop2 — dogfood ae4900eb)
    }
    IChatPeerLinks* links = &chat->links;
    log().info << "bootstrap warm assoc start peer=" << hop.peer_id << " (serial)";
    chat->links.EnsureAssociation(hop.peer_id, [this, links, hop](IChatPeerLinks::LinkRoe assoc) {
      if (!assoc) {
        log().warning << "bootstrap warm assoc miss peer=" << hop.peer_id << " err=" << assoc.error().message;
        return;
      }
      RegisterDialableEndpoint(*links, hop);
      log().info << "bootstrap warm assoc ok peer=" << hop.peer_id;
    });
    return;  // one cold dial at a time
  }
}

void MeshMediaPlane::ReserveOnBootstrapSeeds() {
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    log().warning << "circuit reserve skipped: amp circuit tunnel not started";
    return;
  }
  if (!m->ChatDeps()) {
    log().warning << "circuit reserve skipped: no chat deps";
    return;
  }
  // The reserve path registers + associates itself — do not also Warm in the same post (double dial).
  PostIoOrRun(deferred_.Bind([this]() { ReserveOnBootstrapSeedsOnIo(); }));
}

std::vector<MeshHopCandidate> MeshMediaPlane::OrderedParkSurface(IChatPeerLinks& links) {
  // H011: the same surface the dialer's CollectDialableCircuitRelayIds uses (not seeds-only) —
  // the dialer's sticky reorder can StartBridge any member, so parking must cover it.
  const auto candidates = RendezvousCandidates();
  std::string sticky_r1 = chosen_circuit_r1_;
  if (sticky_r1.empty() && circuit_hop_reach_) {
    sticky_r1 = circuit_hop_reach_->LastGoodRelayPeerKey();
  }
  if (!sticky_r1.empty()) {
    log().info << "circuit rendezvous park sticky R1=" << sticky_r1;
  }
  std::vector<std::string> surface_ids;
  std::unordered_map<std::string, std::string> ma_by_peer;
  for (const auto& hop : candidates) {
    RegisterDialableEndpoint(links, hop);
    surface_ids.push_back(hop.peer_id);
    if (!hop.multiaddr.empty()) {
      ma_by_peer.emplace(hop.peer_id, hop.multiaddr);
    }
  }
  const auto ordered = OrderRendezvousParkAttempts(
      std::move(surface_ids), sticky_r1, [&](const std::string& peer_id) { return links.IsConnected(peer_id); });
  std::vector<MeshHopCandidate> hops;
  hops.reserve(ordered.size());
  for (const std::string& peer_id : ordered) {
    MeshHopCandidate hop;
    hop.peer_id = peer_id;
    if (auto it = ma_by_peer.find(peer_id); it != ma_by_peer.end()) {
      hop.multiaddr = it->second;
    }
    hops.push_back(std::move(hop));
  }
  return hops;
}

void MeshMediaPlane::ReserveOnBootstrapSeedsOnIo() {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    log().warning << "circuit reserve on-io skipped: tunnel not started";
    return;
  }
  auto park = std::make_shared<ColdPark>();
  park->hops = OrderedParkSurface(chat->links);
  const auto connected =
      static_cast<size_t>(std::count_if(park->hops.begin(), park->hops.end(),
                                        [&](const MeshHopCandidate& hop) { return chat->links.IsConnected(hop.peer_id); }));
  park->cold_limit = RendezvousColdDialLimit(park->hops.size(), connected, kCircuitRendezvousParkCoverage,
                                             /*cover_all_remaining=*/true);
  log().info << "circuit rendezvous reserve surface=" << park->hops.size() << " connected=" << connected
             << " cold_limit=" << park->cold_limit << " coverage_k=" << kCircuitRendezvousParkCoverage;
  // Reserve every Connected surface member first (H011 / dogfood 39412f), then dial the rest.
  for (const auto& hop : park->hops) {
    if (!chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint) {
      log().info << "circuit reserve skip peer=" << hop.peer_id << " reason=!endpoint";
    } else if (chat->links.IsConnected(hop.peer_id)) {
      StartReserveOnRelay(hop.peer_id, "circuit reserve");
    }
  }
  ReserveColdSurface(park, 0, 0);
}

void MeshMediaPlane::ReserveColdSurface(const std::shared_ptr<ColdPark>& park, size_t index, size_t cold_started) {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat) {
    return;
  }
  for (; index < park->hops.size() && cold_started < park->cold_limit; ++index) {
    const MeshHopCandidate& hop = park->hops[index];
    if (!chat->links.GetLinkSnapshot(hop.peer_id).has_endpoint || chat->links.IsConnected(hop.peer_id)) {
      continue;  // not dialable, or already reserved in the Connected pass
    }
    IChatPeerLinks* links = &chat->links;
    log().info << "circuit reserve assoc start peer=" << hop.peer_id << " (serial index=" << index
               << " cold=" << cold_started + 1 << "/" << park->cold_limit << ")";
    chat->links.EnsureAssociation(
        hop.peer_id,
        deferred_.Bind([this, park, links, hop, next = index + 1, next_cold = cold_started + 1](IChatPeerLinks::LinkRoe assoc) {
          if (!assoc) {
            log().warning << "circuit reserve assoc miss peer=" << hop.peer_id << " err=" << assoc.error().message;
          } else {
            RegisterDialableEndpoint(*links, hop);
            StartReserveOnRelay(hop.peer_id, "circuit reserve");
          }
          ReserveColdSurface(park, next, next_cold);
        }));
    return;
  }
}

void MeshMediaPlane::StartReserveOnRelay(const std::string& relay, const char* label) {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  if (ReachableOnlyViaCarrier(chat->links, relay)) {
    log().info << label << " skip peer=" << relay << " reason=carrier-only";
    return;
  }
  const std::string what = label;
  const auto id = m->AmpCircuitTunnel()->StartReserve(
      relay,
      deferred_.Bind([this, relay, what](Roe<CircuitTunnelBridgeResult> result) {
        if (!result || !result->ok) {
          // Live Brief may still lack op=reserve (dogfood fd4e3de "unsupported op"). A Connected
          // PeerLink alone is enough for peer-id-only ServeDial — log and keep the link.
          log().warning << what << " miss peer=" << relay << " err="
                        << (!result ? result.error().message : (result->error.empty() ? "rejected" : result->error));
          return;
        }
        log().info << what << " ok peer=" << relay;
      }),
      15000);
  if (!id) {
    log().warning << what << " StartReserve rejected peer=" << relay;
    return;
  }
  log().info << what << " started peer=" << relay;
}

void MeshMediaPlane::PreferLateReserve(const std::string& relay_peer_id) {
  if (relay_peer_id.empty()) {
    return;
  }
  chosen_circuit_r1_ = relay_peer_id;
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted() || !m->ChatDeps()) {
    log().warning << "circuit late-reserve skipped: tunnel not started peer=" << relay_peer_id;
    return;
  }
  PostIoOrRun(deferred_.Bind([this, relay_peer_id]() { PreferLateReserveOnIo(relay_peer_id); }));
}

void MeshMediaPlane::PreferLateReserveOnIo(const std::string& relay_peer_id) {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat || relay_peer_id.empty() || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  // Ensure the chosen R1 has a dialable MA when the surface knows one.
  for (const auto& hop : RendezvousCandidates()) {
    if (hop.peer_id == relay_peer_id) {
      RegisterDialableEndpoint(chat->links, hop);
      break;
    }
  }
  if (!chat->links.GetLinkSnapshot(relay_peer_id).has_endpoint) {
    log().warning << "circuit late-reserve skip peer=" << relay_peer_id << " reason=!endpoint";
    return;
  }
  if (chat->links.IsConnected(relay_peer_id)) {
    StartReserveOnRelay(relay_peer_id, "circuit late-reserve");
    return;
  }
  log().info << "circuit late-reserve assoc start peer=" << relay_peer_id;
  chat->links.EnsureAssociation(relay_peer_id, deferred_.Bind([this, relay_peer_id](IChatPeerLinks::LinkRoe assoc) {
    if (!assoc) {
      log().warning << "circuit late-reserve assoc miss peer=" << relay_peer_id << " err=" << assoc.error().message;
      return;
    }
    StartReserveOnRelay(relay_peer_id, "circuit late-reserve");
  }));
}

// --- park await -----------------------------------------------------------------------------------

void MeshMediaPlane::EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, const int timeout_ms) {
  if (!on_done) {
    return;
  }
  ReserveOnBootstrapSeeds();
  MeshHost* m = mesh();
  if (!m || !m->ChatDeps() || !m->Amp()) {
    on_done(false);
    return;
  }
  auto park = std::make_shared<SeedPark>();
  park->links = &m->Amp()->Runtime().Links();
  park->on_done = std::move(on_done);
  // Prefer Connected on *all* seeds — the dialer's StartBridge may pick hop2 while we only parked
  // hop1 (dogfood ae4900eb / 39412f). The deadline still accepts ≥1.
  if (AllBootstrapSeedsConnectedOnIo()) {
    log().info << "bootstrap seed park ok (all seeds Connected)";
    FinishSeedPark(park, true);
    return;
  }
  const auto ids = BootstrapSeedPeerIds();
  if (ids.empty()) {
    log().warning << "bootstrap seed park timeout (no seed PeerIds)";
    FinishSeedPark(park, false);
    return;
  }
  const std::unordered_set<std::string> seed_ids(ids.begin(), ids.end());
  park->listener_id = park->links->AddPeerConnectedListener(
      deferred_.Bind([this, park, seed_ids](const std::string& peer_id) {
        if (seed_ids.count(peer_id) > 0) {
          PostIoOrRun(deferred_.Bind([this, park]() { CheckSeedPark(park, /*at_deadline=*/false); }));
        }
      }));
  // Connected may have landed between the check and AddPeerConnectedListener.
  PostIoOrRun(deferred_.Bind([this, park]() { CheckSeedPark(park, /*at_deadline=*/false); }));
  // The deadline always answers: after InvalidateAsyncOps (mesh stop) the park is abandoned with
  // `false` instead of checking links that may be gone.
  park->deadline_timer = AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 12000),
      [this, park, alive = deferred_.token(), snap = deferred_.Snapshot()]() {
        if (!DeferredSelf::Alive(alive, snap)) {
          AbandonSeedPark(park);
          return;
        }
        PostIoOrRun(deferred_.Bind([this, park]() { CheckSeedPark(park, /*at_deadline=*/true); }));
      });
}

void MeshMediaPlane::AbandonSeedPark(const std::shared_ptr<SeedPark>& park) {
  if (park->settled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  park->listener_id.store(0);  // the mesh's links (and their listeners) went with the stop
  park->on_done(false);
}

void MeshMediaPlane::CheckSeedPark(const std::shared_ptr<SeedPark>& park, bool at_deadline) {
  if (park->settled.load(std::memory_order_acquire)) {
    return;
  }
  if (AllBootstrapSeedsConnectedOnIo()) {
    log().info << "bootstrap seed park ok (all seeds Connected)";
    FinishSeedPark(park, true);
  } else if (at_deadline) {
    const bool any = AnyBootstrapSeedConnectedOnIo();
    if (any) {
      log().info << "bootstrap seed park ok (partial — deadline with ≥1 Connected)";
    } else {
      log().warning << "bootstrap seed park timeout (no Connected seed)";
    }
    FinishSeedPark(park, any);
  }
}

void MeshMediaPlane::FinishSeedPark(const std::shared_ptr<SeedPark>& park, bool parked) {
  if (park->settled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (const auto id = park->listener_id.exchange(0); id != 0) {
    park->links->RemovePeerConnectedListener(id);
  }
  if (const auto timer = park->deadline_timer.exchange(0); timer != 0) {
    AppRuntime::CancelCoordinatorTimer(timer);
  }
  park->on_done(parked);
}

bool MeshMediaPlane::AwaitCircuitReady(const int timeout_ms) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto parked = std::make_shared<std::atomic<bool>>(false);
  EnsureBootstrapSeedParkedAsync(
      [done, parked](const bool ok) {
        parked->store(ok, std::memory_order_release);
        done->store(true, std::memory_order_release);
      },
      timeout_ms);
  const int budget = timeout_ms > 0 ? timeout_ms : 12000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget + 250);
  AmpParkUntil([done] { return done->load(std::memory_order_acquire); }, deadline, {});
  return parked->load(std::memory_order_acquire);
}

// --- re-park on reconnect -------------------------------------------------------------------------

void MeshMediaPlane::InstallRendezvousReparkListener() {
  RemoveRendezvousReparkListener();
  MeshHost* m = mesh();
  if (!m || !m->Amp() || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  repark_listener_id_ = m->Amp()->Runtime().Links().AddPeerConnectedListener(
      deferred_.Bind([this](const std::string& peer_id) { OnRendezvousPeerReconnected(peer_id); }));
  log().info << "circuit rendezvous re-park listener armed";
}

void MeshMediaPlane::RemoveRendezvousReparkListener() {
  if (repark_listener_id_ == 0) {
    return;
  }
  if (MeshHost* m = mesh(); m && m->Amp()) {
    m->Amp()->Runtime().Links().RemovePeerConnectedListener(repark_listener_id_);
  }
  repark_listener_id_ = 0;
}

void MeshMediaPlane::OnRendezvousPeerReconnected(const std::string& peer_id) {
  MeshHost* m = mesh();
  if (peer_id.empty() || !m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  const auto surface = RendezvousCandidates();
  if (std::none_of(surface.begin(), surface.end(), [&](const MeshHopCandidate& hop) { return hop.peer_id == peer_id; })) {
    return;
  }
  log().info << "circuit rendezvous re-park on reconnect peer=" << peer_id;
  PreferLateReserveOnIo(peer_id);
}

} // namespace pbr
