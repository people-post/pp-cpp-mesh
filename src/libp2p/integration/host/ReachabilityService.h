#pragma once

#include "libp2p/integration/host/Reachability.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace pbr {

class DialBackService;
class NodeRuntime;

/** Async reachability probe orchestration (nr). Thread-safe snapshot reads. */
class ReachabilityService {
public:
  ReachabilityService() = default;

  ReachabilitySnapshot Snapshot() const;

  bool IsProbing() const { return probing_.load(); }

  /** Fire-and-forget probe on the libp2p worker pool (Background lane). */
  void StartProbe(NodeRuntime& runtime, DialBackService& dial_back, bool try_upnp_first);

  /** Block until an in-flight probe completes (pp-node --status). */
  void RunProbeBlocking(NodeRuntime& runtime, DialBackService& dial_back, bool try_upnp_first);

  /** Ops / pp-node --status JSON line. */
  std::string FormatOpsStatusJson() const;

  /** Subscribe to probe completion (UI refresh). Called on worker thread. */
  void SetOnUpdated(std::function<void()> callback);

private:
  void RunProbe(NodeRuntime& runtime, DialBackService& dial_back, bool try_upnp_first);
  void Publish(ReachabilitySnapshot snapshot);

  mutable std::mutex mutex_;
  ReachabilitySnapshot snapshot_;
  std::function<void()> on_updated_;
  std::atomic<bool> probing_{false};
  std::string upnp_external_ip_;
  int upnp_external_port_ = 0;
};

} // namespace pbr
