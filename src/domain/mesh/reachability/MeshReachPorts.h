#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"  // kRealtimeProtocolId
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"  // kMediaRelayProtocolId

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

// Reach ports shared by every media client (calls, broadcast): which mesh keys are dialable /
// connected, and circuit / punch reach to a peer or hop. Feature-neutral (media-client-layers L002).

/** Dial registry surface for hop RegisterEndpoint / IsDialable. */
class IDialRegistry {
public:
  virtual ~IDialRegistry() = default;

  virtual Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) = 0;
  /**
   * Best-first candidate list (B28). Default reverse-registers so Preferred =
   * multiaddrs.front(); PeerSessionDialRegistry uses Amp RegisterEndpoints atomically.
   */
  virtual Roe<void> RegisterEndpoints(const std::string& peer_key,
                                      const std::vector<std::string>& multiaddrs) {
    if (multiaddrs.empty()) {
      return Error("dial registry: no multiaddrs");
    }
    Roe<void> last = {};
    for (auto it = multiaddrs.rbegin(); it != multiaddrs.rend(); ++it) {
      last = RegisterEndpoint(peer_key, *it);
    }
    return last;
  }
  virtual bool IsDialable(const std::string& peer_key) const = 0;
  /** PeerLink Connected — stricter than IsDialable (has_endpoint alone is not enough). */
  virtual bool IsConnected(const std::string& peer_key) const {
    (void)peer_key;
    return false;
  }
  /** Connected over a direct (ADP) link — IsConnected also counts a relay carrier link. */
  virtual bool IsConnectedDirect(const std::string& peer_key) const { return IsConnected(peer_key); }
  /** Kick ADP dial/handshake; optional for fakes. */
  virtual void EnsureAssociation(const std::string& peer_key,
                                 std::function<void(Roe<void>)> on_done) {
    (void)peer_key;
    if (on_done) {
      on_done(Error("ensure association not available"));
    }
  }
  virtual std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const = 0;
  virtual void ClearDialBackoff(const std::string& peer_key) = 0;
  virtual void AbortInflightDial(const std::string& peer_key) = 0;
  /** Close the live link for peer_key so the next dial uses fresh candidates (B39: stale
   *  "connected" link after the peer changed network). Default no-op for fakes. */
  virtual void DropLink(const std::string& /*peer_key*/) {}
  virtual void ClearPeerCircuitHop(const std::string& peer_key) = 0;
  /** True when a nested circuit carrier hop (realtime protocol) is installed for the peer. */
  virtual bool HasPeerCircuitHop(const std::string& peer_key) const {
    (void)peer_key;
    return false;
  }
};

/** L3: circuit bridge fallback when hop PeerId is not directly dialable. */
class ICircuitHopReach {
public:
  virtual ~ICircuitHopReach() = default;
  /** Reach a media_relay hop (topology / prefetch). */
  virtual Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) = 0;
  /** Prefer over sync when MeshPump + PostToIo are available. */
  virtual void TryEnsureHopReachableAsync(const std::string& hop_peer_id,
                                          std::function<void(Roe<void>)> on_done) {
    if (on_done) {
      on_done(TryEnsureHopReachable(hop_peer_id));
    }
  }
  /** Reach a peer (realtime protocol) when not directly dialable: circuit, then punch.
   *  `allow_circuit`: when false, punch only and wait for the peer to connect (the awaiting side
   *  of a reach — the reaching side builds the circuit to our seed reservation). */
  virtual Roe<void> TryEnsurePeerReachable(const std::string& peer_key) = 0;
  virtual void TryEnsurePeerReachableAsync(const std::string& peer_key,
                                                std::function<void(Roe<void>)> on_done,
                                                bool allow_circuit = true) {
    (void)allow_circuit;
    if (on_done) {
      on_done(TryEnsurePeerReachable(peer_key));
    }
  }
  /** L3.25c: punch via circuit R1 as introducer, then demote the circuit hop. */
  virtual Roe<void> TryUpgradeToDirect(const std::string& peer_key) {
    (void)peer_key;
    return Error("circuit upgrade not available");
  }
  virtual void TryUpgradeToDirectAsync(const std::string& peer_key,
                                       std::function<void(Roe<void>)> on_done) {
    if (on_done) {
      on_done(TryUpgradeToDirect(peer_key));
    }
  }
  /** Abort in-flight EnsureViaCircuit / punch chains (ConnectFailed / Leave / teardown). */
  virtual void AbortPending() {}
  /**
   * H011 L3.1b: last relay PeerId that completed a successful StartBridge Install.
   * Empty when none yet. Answerer park may prefer this as sticky.
   */
  virtual std::string LastGoodRelayPeerKey() const { return {}; }
};

/** Amp-only dial registry (PeerLinkManager + AmpCircuitHopRegistry). */
class PeerSessionDialRegistry final : public IDialRegistry {
public:
  PeerSessionDialRegistry() = default;

  void SetAmpLinks(IChatPeerLinks* amp_links) { amp_links_ = amp_links; }
  void SetAmpCircuitHops(AmpCircuitHopRegistry* hops) { amp_hops_ = hops; }
  /** MeshRuntime::PostToIo — mutations must run on the Amp IO strand (MeshPump). */
  void SetPostIo(std::function<void(std::function<void()>)> post_io) { post_io_ = std::move(post_io); }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    if (!amp_links_) {
      return Error("dial registry not available");
    }
    if (!IsAdpMultiaddr(multiaddr)) {
      return Error("dial registry not available");
    }
    auto run = [this, peer_key, multiaddr]() {
      if (!amp_links_) {
        return;
      }
      (void)amp_links_->RegisterEndpoint(peer_key, multiaddr);
      if (auto peer_id = PeerIdFromAdpMultiaddr(multiaddr); peer_id && *peer_id != peer_key) {
        (void)amp_links_->RegisterEndpoint(*peer_id, multiaddr);
      }
    };
    PostAmpIo(std::move(run));
    return {};
  }

  Roe<void> RegisterEndpoints(const std::string& peer_key,
                              const std::vector<std::string>& multiaddrs) override {
    if (!amp_links_) {
      return Error("dial registry not available");
    }
    std::vector<std::string> adp;
    adp.reserve(multiaddrs.size());
    for (const std::string& ma : multiaddrs) {
      if (IsAdpMultiaddr(ma)) {
        adp.push_back(ma);
      }
    }
    if (adp.empty()) {
      return Error("dial registry not available");
    }
    auto run = [this, peer_key, adp]() {
      if (!amp_links_) {
        return;
      }
      (void)amp_links_->RegisterEndpoints(peer_key, adp);
      if (auto peer_id = PeerIdFromAdpMultiaddr(adp.front()); peer_id && *peer_id != peer_key) {
        (void)amp_links_->RegisterEndpoints(*peer_id, adp);
      }
    };
    PostAmpIo(std::move(run));
    return {};
  }

  bool IsDialable(const std::string& peer_key) const override {
    if (amp_links_) {
      if (amp_links_->GetLinkSnapshot(peer_key).has_endpoint) {
        return true;
      }
      // L3.25b: successful punch may leave a Connected PeerLink under PeerId before/without
      // a separate endpoint row — SoftMigrate should still treat that as direct-dialable.
      if (amp_links_->IsConnected(peer_key)) {
        return true;
      }
    }
    // Relay-hop dialability is media-relay-specific. Realtime-protocol circuit hops
    // must not mark a peer dialable for quote/attach (TryEnsurePeerReachable covers
    // peers and is protocol-keyed).
    return amp_hops_ && static_cast<bool>(amp_hops_->Find(peer_key, kMediaRelayProtocolId));
  }

  bool IsConnected(const std::string& peer_key) const override {
    return amp_links_ && amp_links_->IsConnected(peer_key);
  }

  bool IsConnectedDirect(const std::string& peer_key) const override {
    if (!amp_links_) {
      return false;
    }
    // By PeerId: presence prefers the ADP link when both ADP and carrier exist (A024/A026).
    const pp::amp::LinkSnapshotEx by_peer = amp_links_->SnapshotByPeerId(peer_key);
    if (by_peer.transport == pp::amp::TransportClass::Adp &&
        by_peer.base.phase == pp::amp::PeerLinkPhase::Connected) {
      return true;
    }
    // Dial alias (account: …): its own link must not be a carrier.
    return amp_links_->IsConnected(peer_key) && !amp_links_->GetLinkSnapshot(peer_key).carrier_backed;
  }

  void EnsureAssociation(const std::string& peer_key,
                         std::function<void(Roe<void>)> on_done) override {
    auto run = [this, peer_key, on_done = std::move(on_done)]() mutable {
      if (!amp_links_) {
        if (on_done) {
          on_done(Error("dial registry not available"));
        }
        return;
      }
      amp_links_->EnsureAssociation(peer_key, [on_done = std::move(on_done)](IChatPeerLinks::LinkRoe r) {
        if (!on_done) {
          return;
        }
        if (!r) {
          on_done(Error(r.error().message));
          return;
        }
        on_done({});
      });
    };
    PostAmpIo(std::move(run));
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (amp_links_) {
      if (auto amp_ma = amp_links_->PreferredMultiaddr(peer_key)) {
        return amp_ma;
      }
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    PostAmpIo([this, peer_key]() {
      if (amp_links_) {
        amp_links_->ClearDialBackoff(peer_key);
      }
    });
  }

  void AbortInflightDial(const std::string& peer_key) override {
    PostAmpIo([this, peer_key]() {
      if (amp_links_) {
        amp_links_->AbortInflightDial(peer_key);
      }
    });
  }

  void DropLink(const std::string& peer_key) override {
    PostAmpIo([this, peer_key]() {
      if (amp_links_) {
        amp_links_->DropLink(peer_key);
      }
    });
  }

  void ClearPeerCircuitHop(const std::string& peer_key) override {
    if (amp_hops_) {
      amp_hops_->Clear(peer_key, kRealtimeProtocolId);
      amp_hops_->Clear(peer_key, pp::amp::kAmpCircuitCarrierProtocolId);
    }
  }

  bool HasPeerCircuitHop(const std::string& peer_key) const override {
    return amp_hops_ &&
           (static_cast<bool>(amp_hops_->Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId)) ||
            amp_hops_->HasAny(peer_key));
  }

private:
  void PostAmpIo(std::function<void()> task) {
    if (!task) {
      return;
    }
    if (post_io_) {
      post_io_(std::move(task));
      return;
    }
    task();
  }

  IChatPeerLinks* amp_links_ = nullptr;
  AmpCircuitHopRegistry* amp_hops_ = nullptr;
  std::function<void(std::function<void()>)> post_io_;
};

/** Forwards to ConversationsHub / CallStack wiring. */
class CircuitHopReachClient final : public ICircuitHopReach {
public:
  CircuitHopReachClient(std::function<Roe<void>(const std::string&)> try_media_hop_reach,
                        std::function<Roe<void>(const std::string&)> try_peer_reach,
                        std::function<Roe<void>(const std::string&)> try_upgrade = {})
      : try_media_hop_reach_(std::move(try_media_hop_reach)),
        try_peer_reach_(std::move(try_peer_reach)),
        try_upgrade_(std::move(try_upgrade)) {}

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override {
    if (!try_media_hop_reach_) {
      return Error("circuit reach not available");
    }
    return try_media_hop_reach_(hop_peer_id);
  }

  Roe<void> TryEnsurePeerReachable(const std::string& peer_key) override {
    if (!try_peer_reach_) {
      return Error("peer circuit reach not available");
    }
    return try_peer_reach_(peer_key);
  }

  Roe<void> TryUpgradeToDirect(const std::string& peer_key) override {
    if (!try_upgrade_) {
      return Error("circuit upgrade not available");
    }
    return try_upgrade_(peer_key);
  }

private:
  std::function<Roe<void>(const std::string&)> try_media_hop_reach_;
  std::function<Roe<void>(const std::string&)> try_peer_reach_;
  std::function<Roe<void>(const std::string&)> try_upgrade_;
};

} // namespace pbr
