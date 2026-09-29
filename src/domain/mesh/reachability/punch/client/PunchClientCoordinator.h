#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/punch/PunchLinkOps.h"
#include "domain/mesh/reachability/punch/PunchTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Client (initiator) side of Amp coordinated punch (`kAmpPunchProtocolId`, H009 / L3.25a–d): ask an
 * introducer to punch us through to a target (cold, or upgrade over a circuit relay), then burst on
 * its `sync`; or burst candidates exchanged over call signaling (H012).
 *
 * Prefer the *Async calls. Sync Try* park (AmpParkUntil) on a waiter that is the sole Amp driver
 * (test harness); IoPump must not be invoked from punch SM callbacks.
 */
class PunchClientCoordinator {
public:
  using Err = PunchErr;
  using Failure = PunchFailure;
  using PunchRoe = CodedRoe<PunchResult, Err>;
  /** Only for AmpParkUntil in sync Try* (harness sole driver). Empty when MeshPump owns Drive. */
  using IoPump = std::function<void()>;

  explicit PunchClientCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump = {});

  PunchClientCoordinator(const PunchClientCoordinator&) = delete;
  PunchClientCoordinator& operator=(const PunchClientCoordinator&) = delete;

  void Start() { stopped_.store(false, std::memory_order_release); }
  void Stop() { stopped_.store(true, std::memory_order_release); }
  bool IsStarted() const { return !stopped_.load(std::memory_order_acquire); }

  void TryColdPunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                         const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                         int window_ms = 2000);

  void TryUpgradePunchAsync(const std::string& introducer_peer_key, const std::string& target_peer_id,
                            const std::vector<std::string>& my_addrs, std::function<void(PunchRoe)> on_done,
                            int window_ms = 2000);

  /**
   * H012 / L3.25d: burst-dial peer candidates without an Amp introducer Session.
   * Caller exchanged addrs + window over call-control (`call_punch_*`).
   */
  void TrySignalingPunchBurstAsync(const std::vector<std::string>& peer_addrs,
                                   std::function<void(PunchRoe)> on_done, int window_ms = 2000);

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
  /** Non-teardown SM work on the IO strand. */
  void PostStrand(std::function<void()> fn) { runtime_.PostToIo(std::move(fn)); }
  /** Teardown lane — Close / on_done (MeshRuntime::PostDeferred). */
  void PostDeferred(std::function<void()> fn) { runtime_.PostDeferred(std::move(fn)); }

  pp::amp::MeshRuntime& runtime_;
  IoPump io_pump_;
  std::atomic<bool> stopped_{true};
};

} // namespace pbr
