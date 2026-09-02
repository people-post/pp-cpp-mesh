#pragma once

#include "domain/mesh/dht/DhtRateLimiter.h"
#include "domain/mesh/discovery/DirectoryTypes.h"
#include "common/directory/IDirectoryClient.h"
#include "amp/link/PeerLinkManager.h"
#include "common/CodedFailure.h"
#include "common/Error.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Amp L4 directory twin (`/pp-mesh/directory/1.0.0`) — list_mesh_nodes.
 * Server answers from an injected snapshot; client fans out to query_peer_keys.
 *
 * Errors follow docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
 */
class AmpDirectoryService {
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
    NotFound,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using ListRoe = CodedRoe<std::vector<MeshNodeHit>, Err>;
  using RpcRoe = CodedRoe<Object, Err>;

  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpDirectoryService(pp::amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {});
  ~AmpDirectoryService();

  AmpDirectoryService(const AmpDirectoryService&) = delete;
  AmpDirectoryService& operator=(const AmpDirectoryService&) = delete;

  void Configure(AmpDirectoryServiceConfig config);
  void SetNodesProvider(AmpDirectoryNodesProvider provider);
  void SetNodesSnapshot(std::vector<MeshNodeHit> nodes);

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Blocking list against configured query_peer_keys (first success wins). */
  ListRoe ListMeshNodes();

  void ListMeshNodesAsync(std::function<void(ListRoe)> on_done);

private:
  friend struct Impl;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  AmpDirectoryServiceConfig config_;
  AmpDirectoryNodesProvider nodes_provider_;
  std::vector<MeshNodeHit> nodes_snapshot_;
  mutable std::mutex nodes_mutex_;
  DhtRateLimiter inbound_limiter_;
  bool started_ = false;

  std::vector<MeshNodeHit> LocalNodes() const;
  bool AllowInbound(const std::string& remote_peer);
};

/**
 * IDirectoryClient adapter over AmpDirectoryService (N029 nd4).
 * Person lookups fail so FailoverDirectoryClient can fall through to HTTP.
 */
class AmpDirectoryClient : public IDirectoryClient {
public:
  explicit AmpDirectoryClient(AmpDirectoryService& service);

  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<MeshNodeHit>> ListMeshNodes() override;

private:
  AmpDirectoryService& service_;
};

} // namespace pbr
