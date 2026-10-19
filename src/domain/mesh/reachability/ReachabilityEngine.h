#pragma once

#include "domain/mesh/reachability/Reachability.h"
#include "common/Lifetime.h"
#include "common/SerialExecutor.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pp::amp {
class PeerLinkManager;
}

namespace pbr {

class DialBackClient;

/** Inputs for an Amp dial-back reachability probe (D8). */
struct AmpReachabilityProbeDeps {
  pp::amp::PeerLinkManager* links = nullptr;
  DialBackClient* dial_back = nullptr;
  std::string amp_listen_multiaddr;
  std::string local_peer_id;
  /** ADP multiaddrs preferred; TCP bootstrap entries are skipped. */
  std::vector<std::string> bootstrap_peers;
  std::function<void()> io_pump;
  std::function<void(std::function<void()>)> post_worker;
  /** MeshRuntime::PostToIo — prefer ProbeAsync + callbacks over parking a thread. */
  std::function<void(std::function<void()>)> post_io;
  /** MeshRuntime::PostAfter — Amp-clock seed dial deadline. */
  std::function<void(std::chrono::milliseconds, std::function<void()>)> post_after;
  /** The mesh's io lane: where the probe's steps run (null: inline — no Amp, nothing to probe). */
  std::shared_ptr<pp::SerialExecutor> lane;
  bool try_upnp_first = false;
};

/**
 * Async reachability probe orchestration (THREADING.md § Mesh threads). The probe's steps, its
 * result and `on_updated` run on the mesh's io lane (inline without one); UPnP discovery (a blocking
 * ~2 s socket wait) is a `blocking` step that hops back. Snapshot reads are thread-safe. Destroy from
 * off the lane: the destructor drops queued steps and waits for one running.
 */
class ReachabilityEngine {
public:
  ReachabilityEngine() = default;
  ~ReachabilityEngine();
  ReachabilityEngine(const ReachabilityEngine&) = delete;
  ReachabilityEngine& operator=(const ReachabilityEngine&) = delete;

  ReachabilitySnapshot Snapshot() const;

  bool IsProbing() const { return probing_.load(); }

  /** Fire-and-forget Amp dial-back probe (steps on the mesh's io lane). */
  void StartProbe(AmpReachabilityProbeDeps deps);

  /** Block until an in-flight probe completes (pp-node --status). */
  void RunProbeBlocking(AmpReachabilityProbeDeps deps);

  /** Ops / pp-node --status JSON line. */
  std::string FormatOpsStatusJson() const;

  /** Subscribe to probe completion. Runs on the mesh's io lane — consumers hop to their own thread. */
  void SetOnUpdated(std::function<void()> callback);

private:
  void RunProbe(AmpReachabilityProbeDeps deps);
  /** After the (optional) UPnP step: seed dial → dial-back probe. On the lane. */
  void ProbeSeed(AmpReachabilityProbeDeps deps, ReachabilitySnapshot result);
  /** Classify, publish, clear `probing_` — on the lane, whatever thread the probe ended on. */
  void Complete(ReachabilitySnapshot result, bool classify);
  /** `step` on the probe's lane, or now without one; dropped once this engine is gone. */
  void PostStep(std::function<void()> step);
  bool OnLane() const;
  void Publish(ReachabilitySnapshot snapshot);

  mutable std::mutex mutex_;
  ReachabilitySnapshot snapshot_;
  std::function<void()> on_updated_;
  std::atomic<bool> probing_{false};
  /** The running probe's lane (mutex_). */
  std::shared_ptr<pp::SerialExecutor> lane_;
  /** Last UPnP mapping (lane): later probes still offer it as a dial-back target. */
  std::string upnp_external_ip_;
  int upnp_external_port_ = 0;
  /** Posted steps: the destructor drops them and waits for one running. */
  pp::Lifetime self_;
};

} // namespace pbr
