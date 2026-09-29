#pragma once

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

  explicit AmpPunchCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump = {})
      : server_(runtime, io_pump), client_(runtime, io_pump) {}

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

  PunchClientCoordinator& Client() { return client_; }

  void TryColdPunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                         const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                         int window_ms = 2000) {
    client_.TryColdPunchAsync(introducer_peer_key, target_peer_id, my_addrs, std::move(on_done), window_ms);
  }
  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                            int window_ms = 2000) {
    client_.TryUpgradePunchAsync(introducer_peer_key, target_peer_id, my_addrs, std::move(on_done), window_ms);
  }
  void TrySignalingPunchBurstAsync(const std::vector<std::string>& peer_addrs, std::function<void(PunchRoe)> on_done,
                                   int window_ms = 2000) {
    client_.TrySignalingPunchBurstAsync(peer_addrs, std::move(on_done), window_ms);
  }
  PunchRoe TryColdPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                        const std::vector<std::string>& my_addrs, int window_ms = 2000) {
    return client_.TryColdPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms);
  }
  PunchRoe TryUpgradePunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                           const std::vector<std::string>& my_addrs, int window_ms = 2000) {
    return client_.TryUpgradePunch(introducer_peer_key, target_peer_id, my_addrs, window_ms);
  }

private:
  PunchServer server_;
  PunchClientCoordinator client_;
};

} // namespace pbr
