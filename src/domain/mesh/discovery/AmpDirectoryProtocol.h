#pragma once

#include "domain/mesh/discovery/DirectoryTypes.h"
#include "domain/mesh/discovery/client/DirectoryClient.h"
#include "domain/mesh/discovery/serve/DirectoryServer.h"
#include "common/directory/IDirectoryClient.h"
#include "amp/link/MeshRuntime.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp L4 directory twin (`/pp-mesh/directory/1.0.0`) — list_mesh_nodes. One config drives both
 * ends: `serve/DirectoryServer` answers from an injected snapshot, `client/DirectoryClient` fans
 * out to query_peer_keys. MeshHost owns one when Amp is up.
 */
class AmpDirectoryProtocol {
public:
  using Err = DirectoryClient::Err;
  using Failure = DirectoryClient::Failure;
  using ListRoe = DirectoryClient::ListRoe;
  using IoPump = DirectoryClient::IoPump;
  using WorkerPost = DirectoryServer::WorkerPost;

  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
    return DirectoryClient::WrapLinkFailure(child);
  }

  AmpDirectoryProtocol(pp::amp::MeshRuntime& runtime, IoPump io_pump = {}, WorkerPost post_worker = {});

  AmpDirectoryProtocol(const AmpDirectoryProtocol&) = delete;
  AmpDirectoryProtocol& operator=(const AmpDirectoryProtocol&) = delete;

  void Configure(AmpDirectoryProtocolConfig config);
  void SetNodesProvider(AmpDirectoryNodesProvider provider) { server_.SetNodesProvider(std::move(provider)); }
  void SetNodesSnapshot(std::vector<MeshNodeHit> nodes) { server_.SetNodesSnapshot(std::move(nodes)); }

  void Start();
  void Stop();
  bool IsStarted() const { return server_.IsStarted(); }

  /** Blocking list against configured query_peer_keys (first success wins). */
  ListRoe ListMeshNodes() { return client_.ListMeshNodes(); }
  void ListMeshNodesAsync(std::function<void(ListRoe)> on_done) { client_.ListMeshNodesAsync(std::move(on_done)); }

private:
  DirectoryServer server_;
  DirectoryClient client_;
};

/**
 * IDirectoryClient adapter over AmpDirectoryProtocol (N029 nd4).
 * Person lookups fail so FailoverDirectoryClient can fall through to HTTP.
 */
class AmpDirectoryClient : public IDirectoryClient {
public:
  explicit AmpDirectoryClient(AmpDirectoryProtocol& service);

  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<MeshNodeHit>> ListMeshNodes() override;

private:
  AmpDirectoryProtocol& service_;
};

} // namespace pbr
