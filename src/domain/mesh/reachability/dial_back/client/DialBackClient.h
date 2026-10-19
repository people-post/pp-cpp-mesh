#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/dial_back/DialBackTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Client side of Amp dial-back (`/pp-browser/reach/1.0.0`, D8): ask a seed to dial our advertised
 * ADP listen multiaddrs, for reachability chrome.
 *
 * Prefer ProbeAsync (A022-style). Sync Probe parks until done; with MeshPump running leave IoPump
 * empty so waiters do not Tick. Channel-open / deadline polls use MeshRuntime::PostToIo / PostAfter.
 */
class DialBackClient {
public:
  using Err = DialBackErr;
  using Failure = DialBackFailure;
  using ProbeRoe = CodedRoe<DialBackProbeResult, Err>;
  using IoPump = std::function<void()>;

  /** Map immediate link-manager failure → dial-back Err (never inspect ADP/PeerLink codes). */
  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
    return WrapDialBackLinkFailure(child);
  }

  explicit DialBackClient(pp::amp::MeshRuntime& runtime, IoPump io_pump = {});

  DialBackClient(const DialBackClient&) = delete;
  DialBackClient& operator=(const DialBackClient&) = delete;

  void Start() { started_ = true; }
  void Stop() { started_ = false; }
  bool IsStarted() const { return started_; }

  /**
   * Ask `seed_peer_key` (must have a registered ADP endpoint) to dial `target_multiaddrs`.
   * Non-blocking; completion via `on_done` (may run on Amp io or inline on the caller).
   */
  void ProbeAsync(const std::string& seed_peer_key, const std::vector<std::string>& target_multiaddrs,
                  std::function<void(ProbeRoe)> on_done, int timeout_ms = 8000);

  /** Blocks until ProbeAsync settles (tests / reachability worker). */
  ProbeRoe Probe(const std::string& seed_peer_key, const std::vector<std::string>& target_multiaddrs,
                 int timeout_ms = 8000);

private:
  pp::amp::MeshRuntime& runtime_;
  IoPump io_pump_;
  bool started_ = false;
};

} // namespace pbr
