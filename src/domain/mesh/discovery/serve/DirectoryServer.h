#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/dht/DhtRateLimiter.h"
#include "domain/mesh/discovery/DirectoryTypes.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {

/**
 * Serving side of the Amp directory twin (`/pp-mesh/directory/1.0.0`): answers `ping` and
 * `list_mesh_nodes` from an injected provider / snapshot, rate limited per remote peer.
 */
class DirectoryServer {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;

  explicit DirectoryServer(pp::amp::MeshRuntime& runtime, WorkerPost post_worker = {});
  ~DirectoryServer();

  DirectoryServer(const DirectoryServer&) = delete;
  DirectoryServer& operator=(const DirectoryServer&) = delete;

  /** Uses local_peer_id and the inbound rate limits. */
  void Configure(const AmpDirectoryProtocolConfig& config);
  void SetNodesProvider(AmpDirectoryNodesProvider provider);
  void SetNodesSnapshot(std::vector<MeshNodeHit> nodes);

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
  bool started_ = false;
};

} // namespace pbr
