#pragma once

#include "lib/amp/link/PeerLinkManager.h"
#include "base/p2p/DialBackTypes.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp L4 dial-back (`/pp-browser/dial-back/1.0.0`) for reachability chrome (D8).
 * Client asks a seed to dial advertised ADP listen multiaddrs; seed replies with ok/dialed/error.
 */
class AmpDialBackService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpDialBackService(amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {});
  ~AmpDialBackService();

  AmpDialBackService(const AmpDialBackService&) = delete;
  AmpDialBackService& operator=(const AmpDialBackService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /**
   * Ask `seed_peer_key` (must have a registered ADP endpoint) to dial `target_multiaddrs`.
   * Blocks with IoPump until response or timeout.
   */
  Roe<DialBackProbeResult> Probe(const std::string& seed_peer_key,
                                 const std::vector<std::string>& target_multiaddrs,
                                 int timeout_ms = 8000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
