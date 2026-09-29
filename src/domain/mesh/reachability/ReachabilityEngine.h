#pragma once

#include "domain/mesh/reachability/Reachability.h"

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
  bool try_upnp_first = false;
};

/**
 * Async reachability probe orchestration (thread-ownership t3-2c). The probe's steps, its result
 * and `on_updated` run on the Connectivity owner (inline without a runtime); UPnP discovery (a
 * blocking ~2 s socket wait) is a worker step; link work completes on Amp IO and hops back.
 * Snapshot reads are thread-safe. Destroy from above the owner (T004): the destructor retires the
 * engine on the owner so no late step touches it.
 */
class ReachabilityEngine {
public:
  ReachabilityEngine() = default;
  ~ReachabilityEngine();
  ReachabilityEngine(const ReachabilityEngine&) = delete;
  ReachabilityEngine& operator=(const ReachabilityEngine&) = delete;

  ReachabilitySnapshot Snapshot() const;

  bool IsProbing() const { return probing_.load(); }

  /** Fire-and-forget Amp dial-back probe (steps on the Connectivity owner). */
  void StartProbe(AmpReachabilityProbeDeps deps);

  /** Block until an in-flight probe completes (pp-node --status). */
  void RunProbeBlocking(AmpReachabilityProbeDeps deps);

  /** Ops / pp-node --status JSON line. */
  std::string FormatOpsStatusJson() const;

  /** Subscribe to probe completion. Runs on the Connectivity owner — consumers hop to their own. */
  void SetOnUpdated(std::function<void()> callback);

private:
  void RunProbe(AmpReachabilityProbeDeps deps);
  /** After the (optional) UPnP step: seed dial → dial-back probe. Owner. */
  void ProbeSeed(AmpReachabilityProbeDeps deps, ReachabilitySnapshot result);
  /** Classify, publish, clear `probing_` — on the owner, whatever thread the probe ended on. */
  void Complete(ReachabilitySnapshot result, bool classify);
  void Publish(ReachabilitySnapshot snapshot);

  mutable std::mutex mutex_;
  ReachabilitySnapshot snapshot_;
  std::function<void()> on_updated_;
  std::atomic<bool> probing_{false};
  /** Last UPnP mapping (owner): later probes still offer it as a dial-back target. */
  std::string upnp_external_ip_;
  int upnp_external_port_ = 0;
  /** Cleared on the owner by the destructor: posted steps for a retired engine drop. */
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pbr
