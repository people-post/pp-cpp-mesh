#pragma once

#include "domain/mesh/reachability/Reachability.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pp::amp {
class PeerLinkManager;
}

namespace pbr {

class AmpDialBackProtocol;

/** Inputs for an Amp dial-back reachability probe (D8). */
struct AmpReachabilityProbeDeps {
  pp::amp::PeerLinkManager* links = nullptr;
  AmpDialBackProtocol* dial_back = nullptr;
  std::string amp_listen_multiaddr;
  std::string local_peer_id;
  /** ADP multiaddrs preferred; TCP bootstrap entries are skipped. */
  std::vector<std::string> bootstrap_peers;
  std::function<void()> io_pump;
  std::function<void(std::function<void()>)> post_worker;
  bool try_upnp_first = false;
};

/** Async reachability probe orchestration. Thread-safe snapshot reads. */
class ReachabilityEngine {
public:
  ReachabilityEngine() = default;

  ReachabilitySnapshot Snapshot() const;

  bool IsProbing() const { return probing_.load(); }

  /** Fire-and-forget Amp dial-back probe on a worker (or inline if no post_worker). */
  void StartProbe(AmpReachabilityProbeDeps deps);

  /** Block until an in-flight probe completes (pp-node --status). */
  void RunProbeBlocking(AmpReachabilityProbeDeps deps);

  /** Ops / pp-node --status JSON line. */
  std::string FormatOpsStatusJson() const;

  /** Subscribe to probe completion (UI refresh). Called on worker thread. */
  void SetOnUpdated(std::function<void()> callback);

private:
  void RunProbe(AmpReachabilityProbeDeps deps);
  void Publish(ReachabilitySnapshot snapshot);

  mutable std::mutex mutex_;
  ReachabilitySnapshot snapshot_;
  std::function<void()> on_updated_;
  std::atomic<bool> probing_{false};
  std::string upnp_external_ip_;
  int upnp_external_port_ = 0;
};

} // namespace pbr
