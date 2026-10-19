#pragma once

#include "common/metrics/MetricsRegistry.h"
#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/punch/PunchLinkOps.h"
#include "domain/mesh/reachability/punch/PunchTypes.h"
#include "domain/mesh/reachability/punch/client/PunchClientCoordinator.h"
#include "domain/mesh/reachability/punch/serve/PunchServer.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp Coordinated Punch L4 (`/pp-browser/reach/1.0.0` family, `kAmpPunchProtocolId`) — H009 /
 * L3.25a–d. A node plays every role: `serve/PunchServer` is the introducer and the target,
 * `client/PunchClientCoordinator` the initiator; this owner shares the local candidate addresses
 * (offered as a target, read by the media plane). Dual-dial election is PeerLinkManager A026;
 * loser teardown is parent-owned A027. MeshHost owns one when Amp is up.
 */
class AmpPunchCoordinator {
public:
  using Err = PunchErr;
  using Failure = PunchFailure;
  using PunchRoe = PunchClientCoordinator::PunchRoe;
  using IoPump = PunchClientCoordinator::IoPump;

  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
    return WrapPunchLinkFailure(child);
  }

  /** `metrics`: where it records (its MeshHost's registry). */
  explicit AmpPunchCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump = {},
                               MetricsRegistry& metrics = MetricsRegistry::Global())
      : metrics_(metrics), server_(runtime, io_pump, metrics), client_(runtime, io_pump) {
    RegisterPunchMetrics();
  }

  AmpPunchCoordinator(const AmpPunchCoordinator&) = delete;
  AmpPunchCoordinator& operator=(const AmpPunchCoordinator&) = delete;

  void Start() {
    server_.Start();
    client_.Start();
  }
  void Stop() {
    client_.Stop();
    server_.Stop();
  }
  bool IsStarted() const { return server_.IsStarted(); }

  void SetLocalCandidateAddrs(std::vector<std::string> addrs) { server_.SetLocalCandidateAddrs(std::move(addrs)); }
  const std::vector<std::string>& LocalCandidateAddrs() const { return server_.LocalCandidateAddrs(); }
  void SetAddressDisclosure(const AddressDisclosureGate* gate) { server_.SetAddressDisclosure(gate); }

  PunchClientCoordinator& Client() { return client_; }

  void TryColdPunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                         const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                         int window_ms = 2000) {
    client_.TryColdPunchAsync(introducer_peer_key, target_peer_id, my_addrs, Counted("cold", std::move(on_done)),
                              window_ms);
  }
  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                            int window_ms = 2000) {
    client_.TryUpgradePunchAsync(introducer_peer_key, target_peer_id, my_addrs,
                                 Counted("upgrade", std::move(on_done)), window_ms);
  }
  void TrySignalingPunchBurstAsync(const std::vector<std::string>& peer_addrs, std::function<void(PunchRoe)> on_done,
                                   int window_ms = 2000) {
    client_.TrySignalingPunchBurstAsync(peer_addrs, Counted("signaling", std::move(on_done)), window_ms);
  }
  PunchRoe TryColdPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                        const std::vector<std::string>& my_addrs, int window_ms = 2000) {
    PunchRoe result = client_.TryColdPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms);
    CountPunch(metrics_, "cold", result);
    return result;
  }
  PunchRoe TryUpgradePunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                           const std::vector<std::string>& my_addrs, int window_ms = 2000) {
    PunchRoe result = client_.TryUpgradePunch(introducer_peer_key, target_peer_id, my_addrs, window_ms);
    CountPunch(metrics_, "upgrade", result);
    return result;
  }

private:
  /** Every punch series exists (at 0) from the start. */
  void RegisterPunchMetrics() {
    MetricsRegistry& r = metrics_;
    for (const char* kind : {"cold", "upgrade", "signaling"}) {
      for (const char* result : {"ok", "failed"}) {
        (void)r.Counter("pp_punch_attempts_total", "Hole punches this node started, by kind and result.",
                        {{"kind", kind}, {"result", result}});
      }
    }
    for (const char* role : {"introducer", "target", "target_declined"}) {
      (void)r.Counter("pp_punch_served_total", "Punch requests this node served, by role.", {{"role", role}});
    }
  }
  /** pp_punch_attempts_total{kind, result} (docs/contracts/NODE_METRICS.md § Reachability). */
  static void CountPunch(MetricsRegistry& metrics, const char* kind, PunchRoe& result) {
    const bool ok = result && result->ok;
    metrics
        .Counter("pp_punch_attempts_total", "Hole punches this node started, by kind and result.",
                 {{"kind", kind}, {"result", ok ? "ok" : "failed"}})
        .Inc();
  }
  /** The completion may land after this coordinator is gone: it holds the registry, not `this`. */
  std::function<void(PunchRoe)> Counted(const char* kind, std::function<void(PunchRoe)> on_done) {
    return [metrics = &metrics_, kind, on_done = std::move(on_done)](PunchRoe result) {
      CountPunch(*metrics, kind, result);
      if (on_done) {
        on_done(std::move(result));
      }
    };
  }

  /** Outlives the mesh (its MeshHost's registry). */
  MetricsRegistry& metrics_;
  PunchServer server_;
  PunchClientCoordinator client_;
};

} // namespace pbr
