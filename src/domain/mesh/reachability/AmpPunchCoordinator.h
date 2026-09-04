#pragma once

#include "amp/link/PeerLinkManager.h"
#include "domain/mesh/reachability/PunchTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp Coordinated Punch L4 (`/pp-browser/amp-punch/1.0.0`) — H009 / L3.25a–c.
 *
 * L3.25a–c: cold/upgrade punch — connect/offer/candidates/sync + burst; upgrade uses circuit R1 as introducer.
 * Dual-dial election is PeerLinkManager A026; loser teardown is parent-owned A027.
 */
class AmpPunchCoordinator {
public:
  enum class Err : int32_t {
    Ok = 0,
    NotStarted,
    EndpointNotRegistered,
    InvalidRequest,
    LinkFailed,
    Timeout,
    ChannelFailed,
    ProtocolError,
    PunchFailed,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using PunchRoe = CodedRoe<PunchResult, Err>;

  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpPunchCoordinator(pp::amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {});
  ~AmpPunchCoordinator();

  AmpPunchCoordinator(const AmpPunchCoordinator&) = delete;
  AmpPunchCoordinator& operator=(const AmpPunchCoordinator&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  void SetLocalCandidateAddrs(std::vector<std::string> addrs);
  const std::vector<std::string>& LocalCandidateAddrs() const { return local_addrs_; }

  PunchRoe TryColdPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                        const std::vector<std::string>& my_addrs, int window_ms = 2000);

  /** L3.25c: same wire as cold punch with reason=upgrade (R1 / circuit relay as introducer). */
  PunchRoe TryUpgradePunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                           const std::vector<std::string>& my_addrs, int window_ms = 2000);

private:
  PunchRoe RunPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                    const std::vector<std::string>& my_addrs, int window_ms, const std::string& reason);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  std::vector<std::string> local_addrs_;
  bool started_ = false;
};

} // namespace pbr
