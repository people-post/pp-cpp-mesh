#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/punch/PunchLinkOps.h"
#include "domain/mesh/reachability/punch/PunchTypes.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Serving side of Amp coordinated punch (`kAmpPunchProtocolId`, H009 / L3.25a–c), both inbound
 * roles:
 * - introducer: an initiator's `connect` → open a punch channel to the target, `offer`, collect its
 *   candidates, then `sync` both ends (each led by the address this node observes for it);
 * - target: an introducer's `offer` → reply with our candidates, then on `sync` burst-dial the
 *   initiator and report the winner.
 * Strand model as AmpPunchCoordinator (THREADING.md): frame handlers only PostToIo; teardown via
 * PostDeferred; never Tick / Drive from SM work.
 */
class PunchServer {
public:
  /** Only for AmpScheduleWhenChannelOpen parks (harness sole driver). Empty when MeshPump owns Drive. */
  using IoPump = std::function<void()>;

  explicit PunchServer(pp::amp::MeshRuntime& runtime, IoPump io_pump = {});
  ~PunchServer();

  PunchServer(const PunchServer&) = delete;
  PunchServer& operator=(const PunchServer&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Candidates this node offers as a punch target (sanitized). */
  void SetLocalCandidateAddrs(std::vector<std::string> addrs);
  const std::vector<std::string>& LocalCandidateAddrs() const { return local_addrs_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
  std::vector<std::string> local_addrs_;
  bool started_ = false;
};

} // namespace pbr
