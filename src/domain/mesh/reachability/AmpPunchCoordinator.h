#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/PunchBurst.h"
#include "domain/mesh/reachability/PunchTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include "amp/L3/ChannelSession.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp Coordinated Punch L4 (`/pp-browser/reach/1.0.0`) — H009 / L3.25a–c.
 *
 * L3.25a–c: cold/upgrade punch — connect/offer/candidates/sync + burst; upgrade uses circuit R1 as introducer.
 * Dual-dial election is PeerLinkManager A026; loser teardown is parent-owned A027.
 *
 * Strand model (THREADING.md — exclusive Amp Drive):
 * - Constructed on MeshRuntime&; PostToIo / PostDeferred / PostAfter / BurstDial come from runtime.
 * - Mux frame handlers only PostToIo. Never call Tick/Drive/IoPump from SM work.
 * - Sync-window burst via MeshRuntime::BurstDial.
 * - AbortInflightDial + session Close + on_done via PostDeferred.
 * - Prefer TryColdPunchAsync. Sync Try* may AmpParkUntil on a waiter that is the sole Amp driver
 *   (test harness); IoPump must not be invoked from punch SM callbacks.
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

  /** Only for AmpParkUntil in sync Try* (harness sole driver). Empty when MeshPump owns Drive. */
  using IoPump = std::function<void()>;

  AmpPunchCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump = {});
  ~AmpPunchCoordinator();

  AmpPunchCoordinator(const AmpPunchCoordinator&) = delete;
  AmpPunchCoordinator& operator=(const AmpPunchCoordinator&) = delete;

  void Start();

  using ProbeInbound = std::function<void(std::shared_ptr<pp::amp::ChannelSession>,
                                        std::vector<uint8_t>)>;
  void SetProbeInbound(ProbeInbound handler);
  void Stop();
  bool IsStarted() const { return started_; }

  void SetLocalCandidateAddrs(std::vector<std::string> addrs);
  const std::vector<std::string>& LocalCandidateAddrs() const { return local_addrs_; }

  void TryColdPunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                         const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                         int window_ms = 2000);

  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                            int window_ms = 2000);

  PunchRoe TryColdPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                        const std::vector<std::string>& my_addrs, int window_ms = 2000);

  /** L3.25c: same wire as cold punch with reason=upgrade (R1 / circuit relay as introducer). */
  PunchRoe TryUpgradePunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                           const std::vector<std::string>& my_addrs, int window_ms = 2000);

private:
  void RunPunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                     const std::vector<std::string>& my_addrs, int window_ms, const std::string& reason,
                     std::function<void(PunchRoe)> on_done);
  PunchRoe RunPunch(const std::string& introducer_peer_key, const std::string& target_peer_id,
                    const std::vector<std::string>& my_addrs, int window_ms, const std::string& reason);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
  IoPump io_pump_;
  std::vector<std::string> local_addrs_;
  bool started_ = false;
};

} // namespace pbr
