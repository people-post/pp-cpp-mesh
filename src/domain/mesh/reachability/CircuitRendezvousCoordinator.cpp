#include "domain/mesh/reachability/CircuitRendezvousCoordinator.h"

#include "common/directory/MeshHopDial.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/circuit/CircuitRendezvousPolicy.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
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

void RegisterDialableEndpoint(IChatPeerLinks& links, const MeshHopCandidate& hop) {
  if (!hop.peer_id.empty() && !hop.multiaddr.empty() && IsAdpMultiaddr(hop.multiaddr) &&
      CircuitHopDialBookAllowsRegister(hop.multiaddr)) {
    (void)links.RegisterEndpoint(hop.peer_id, hop.multiaddr);
  }
}

} // namespace

/** One EnsureBootstrapSeedParkedAsync wait: settles once (listener, deadline or immediate check). */
struct CircuitRendezvousCoordinator::SeedPark {
  std::atomic<bool> settled{false};
  pp::amp::PeerLinkManager* links = nullptr;
  std::atomic<pp::amp::PeerLinkManager::PeerConnectedListenerId> listener_id{0};
  std::atomic<uint64_t> deadline_timer{0};
  std::function<void(bool)> on_done;
};

/** Serial cold dial + reserve over the rest of the rendezvous surface (bounded by cold_limit). */
struct CircuitRendezvousCoordinator::ColdPark {
  std::vector<MeshHopCandidate> hops;
  size_t cold_limit = 0;
};

CircuitRendezvousCoordinator::CircuitRendezvousCoordinator() {
  redirectLogger("CircuitRendezvous");
}

CircuitRendezvousCoordinator::~CircuitRendezvousCoordinator() {
  Invalidate();
}

void CircuitRendezvousCoordinator::SetDeps(CircuitRendezvousDeps deps) {
  deps_ = std::move(deps);
}

void CircuitRendezvousCoordinator::NoteChosenRelay(const std::string& relay_peer_id) {
  if (relay_peer_id.empty()) {
    return;
  }
  // The sticky R1 is read while ordering the park surface, on the IO strand: written there too.
  PostIoOrRun(deferred_.Bind([this, relay_peer_id]() { chosen_circuit_r1_ = relay_peer_id; }));
}

void CircuitRendezvousCoordinator::Invalidate() {
  RemoveReparkListener();
  deferred_.Invalidate();
}

void CircuitRendezvousCoordinator::Clear() {
  Invalidate();
  chosen_circuit_r1_.clear();
}

std::vector<MeshHopCandidate> CircuitRendezvousCoordinator::RendezvousCandidates(const std::string& exclude_peer_id) const {
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

std::vector<std::string> CircuitRendezvousCoordinator::DialableRelayIds(const std::string& exclude_peer_id) const {
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

std::vector<std::string> CircuitRendezvousCoordinator::BootstrapSeedPeerIds() const {
  std::vector<std::string> out;
  for (const auto& hop : deps_.bootstrap_seeds ? deps_.bootstrap_seeds() : std::vector<MeshHopCandidate>{}) {
    if (!hop.peer_id.empty()) {
      out.push_back(hop.peer_id);
    }
  }
  return out;
}

bool CircuitRendezvousCoordinator::AnyBootstrapSeedConnectedOnIo() const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (!chat) {
    return false;
  }
  const auto ids = BootstrapSeedPeerIds();
  return std::any_of(ids.begin(), ids.end(), [&](const std::string& id) { return chat->links.IsConnected(id); });
}

bool CircuitRendezvousCoordinator::AllBootstrapSeedsConnectedOnIo() const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  const auto ids = BootstrapSeedPeerIds();
  if (!chat || ids.empty()) {
    return false;
  }
  return std::all_of(ids.begin(), ids.end(), [&](const std::string& id) { return chat->links.IsConnected(id); });
}

void CircuitRendezvousCoordinator::WarmBootstrapSeedSessions() {
  MeshHost* m = mesh();
  if (!m || !m->ChatDeps()) {
    return;
  }
  // EnsureAssociation / RegisterEndpoint run on Amp IO (dogfood 085210 Coordinator vs MeshPump).
  PostIoOrRun(deferred_.Bind([this]() { WarmBootstrapSeedSessionsOnIo(); }));
}

void CircuitRendezvousCoordinator::WarmBootstrapSeedSessionsOnIo() {
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

void CircuitRendezvousCoordinator::ReserveOnBootstrapSeeds() {
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

std::vector<MeshHopCandidate> CircuitRendezvousCoordinator::OrderedParkSurface(IChatPeerLinks& links) {
  // H011: the same surface the dialer's CollectDialableCircuitRelayIds uses (not seeds-only) —
  // the dialer's sticky reorder can StartBridge any member, so parking must cover it.
  const auto candidates = RendezvousCandidates();
  std::string sticky_r1 = chosen_circuit_r1_;
  if (sticky_r1.empty() && deps_.last_good_relay) {
    sticky_r1 = deps_.last_good_relay();
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

void CircuitRendezvousCoordinator::ReserveOnBootstrapSeedsOnIo() {
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

void CircuitRendezvousCoordinator::ReserveColdSurface(const std::shared_ptr<ColdPark>& park, size_t index, size_t cold_started) {
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

void CircuitRendezvousCoordinator::StartReserveOnRelay(const std::string& relay, const char* label) {
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

void CircuitRendezvousCoordinator::PreferLateReserve(const std::string& relay_peer_id) {
  if (relay_peer_id.empty()) {
    return;
  }
  NoteChosenRelay(relay_peer_id);
  MeshHost* m = mesh();
  if (!m || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted() || !m->ChatDeps()) {
    log().warning << "circuit late-reserve skipped: tunnel not started peer=" << relay_peer_id;
    return;
  }
  PostIoOrRun(deferred_.Bind([this, relay_peer_id]() { PreferLateReserveOnIo(relay_peer_id); }));
}

void CircuitRendezvousCoordinator::PreferLateReserveOnIo(const std::string& relay_peer_id) {
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

void CircuitRendezvousCoordinator::EnsureBootstrapSeedParkedAsync(std::function<void(bool parked)> on_done, const int timeout_ms) {
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

void CircuitRendezvousCoordinator::AbandonSeedPark(const std::shared_ptr<SeedPark>& park) {
  if (park->settled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  park->listener_id.store(0);  // the mesh's links (and their listeners) went with the stop
  park->on_done(false);
}

void CircuitRendezvousCoordinator::CheckSeedPark(const std::shared_ptr<SeedPark>& park, bool at_deadline) {
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

void CircuitRendezvousCoordinator::FinishSeedPark(const std::shared_ptr<SeedPark>& park, bool parked) {
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

void CircuitRendezvousCoordinator::InstallReparkListener() {
  RemoveReparkListener();
  MeshHost* m = mesh();
  if (!m || !m->Amp() || !m->AmpCircuitTunnel() || !m->AmpCircuitTunnel()->IsStarted()) {
    return;
  }
  repark_listener_id_ = m->Amp()->Runtime().Links().AddPeerConnectedListener(
      deferred_.Bind([this](const std::string& peer_id) { OnRendezvousPeerReconnected(peer_id); }));
  log().info << "circuit rendezvous re-park listener armed";
}

void CircuitRendezvousCoordinator::RemoveReparkListener() {
  if (repark_listener_id_ == 0) {
    return;
  }
  if (MeshHost* m = mesh(); m && m->Amp()) {
    m->Amp()->Runtime().Links().RemovePeerConnectedListener(repark_listener_id_);
  }
  repark_listener_id_ = 0;
}

void CircuitRendezvousCoordinator::OnRendezvousPeerReconnected(const std::string& peer_id) {
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

void CircuitRendezvousCoordinator::PostIoOrRun(std::function<void()> task) const {
  MeshHost* m = mesh();
  auto chat = m ? m->ChatDeps() : std::nullopt;
  if (chat && chat->io.post_io) {
    chat->io.post_io(std::move(task));
  } else {
    task();
  }
}

} // namespace pbr
