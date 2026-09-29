#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/discovery/DirectoryTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Client side of the Amp directory twin (`/pp-mesh/directory/1.0.0`): `list_mesh_nodes` fanned
 * out to the configured query peers, first success wins.
 *
 * Errors follow docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
 */
class DirectoryClient {
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
  using IoPump = std::function<void()>;

  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  explicit DirectoryClient(pp::amp::MeshRuntime& runtime, IoPump io_pump = {});

  DirectoryClient(const DirectoryClient&) = delete;
  DirectoryClient& operator=(const DirectoryClient&) = delete;

  /** Uses query_peer_keys and rpc_timeout_ms. */
  void Configure(const AmpDirectoryProtocolConfig& config);

  void Start();
  void Stop();
  bool IsStarted() const { return !stopped_.load(std::memory_order_acquire); }

  /** Blocking list against configured query_peer_keys (first success wins). */
  ListRoe ListMeshNodes();
  void ListMeshNodesAsync(std::function<void(ListRoe)> on_done);

private:
  void Rpc(const std::string& peer_key, Object request, std::function<void(RpcRoe)> on_response);

  pp::amp::MeshRuntime& runtime_;
  IoPump io_pump_;
  AmpDirectoryProtocolConfig config_;
  std::atomic<bool> stopped_{true};
};

} // namespace pbr
